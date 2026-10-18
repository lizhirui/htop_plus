/* htop - released under the GNU GPLv2+, see COPYING. */

#include "config.h" // IWYU pragma: keep

#include "linux/CableConfig.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "XUtils.h"


static bool CableConfig_error(char* error, size_t size, const char* message) {
   xSnprintf(error, size, "%s", message);
   return false;
}

static bool CableConfig_string(struct json_object* root, const char* key, char* out, size_t size) {
   struct json_object* value;
   out[0] = '\0';
   if (!json_object_object_get_ex(root, key, &value) || !value)
      return true;
   if (!json_object_is_type(value, json_type_string))
      return false;
   const char* text = json_object_get_string(value);
   size_t length = strlen(text);
   if (!length || length >= size || length != (size_t)json_object_get_string_len(value))
      return false;
   bool nonblank = false;
   for (size_t i = 0; i < length; i++)
      if (!isspace((unsigned char)text[i]))
         nonblank = true;
   if (!nonblank)
      return false;
   xSnprintf(out, size, "%s", text);
   return true;
}

static bool CableConfig_hex(char* text) {
   if (!*text)
      return true;
   if (strlen(text) != 4)
      return false;
   for (int i = 0; i < 4; i++) {
      if (!isxdigit((unsigned char)text[i]))
         return false;
      text[i] = tolower((unsigned char)text[i]);
   }
   return true;
}

bool CableConfig_load(const char* path, CableConfig* config, char* error, size_t size) {
   *config = (CableConfig) { .timeout = 4.0 };
   FILE* file = fopen(path, "r");
   if (!file)
      return CableConfig_error(error, size, "Cable: cannot read nvpowermon config");
   char text[8192];
   size_t length = fread(text, 1, sizeof(text), file);
   bool failed = ferror(file) || length >= sizeof(text);
   fclose(file);
   if (failed)
      return CableConfig_error(error, size, "Cable: config read failed / too large");
   text[length] = '\0';
   struct json_tokener* tokener = json_tokener_new();
   if (!tokener)
      return CableConfig_error(error, size, "Cable: cannot allocate JSON parser");
   json_tokener_set_flags(tokener, JSON_TOKENER_STRICT);
   struct json_object* root = json_tokener_parse_ex(tokener, text, length + 1);
   bool valid = json_tokener_get_error(tokener) == json_tokener_success &&
                root && json_object_is_type(root, json_type_object) && strlen(text) == length;
   json_tokener_free(tokener);
   if (!valid) {
      json_object_put(root);
      return CableConfig_error(error, size, "Cable: invalid JSON config");
   }
   json_object_object_foreach(root, key, member) {
      (void)member;
      if (!String_eq(key, "usb_serial") && !String_eq(key, "usb_vid") &&
          !String_eq(key, "usb_pid") && !String_eq(key, "port") &&
          !String_eq(key, "baudrate") && !String_eq(key, "timeout_seconds"))
         valid = false;
   }
   valid = valid && CableConfig_string(root, "usb_serial", config->serial, sizeof(config->serial)) &&
           CableConfig_string(root, "usb_vid", config->vid, sizeof(config->vid)) &&
           CableConfig_string(root, "usb_pid", config->pid, sizeof(config->pid)) &&
           CableConfig_string(root, "port", config->port, sizeof(config->port)) &&
           CableConfig_hex(config->vid) && CableConfig_hex(config->pid) &&
           (*config->serial || *config->port) &&
           (!*config->port || String_startsWith(config->port, "/dev/"));
   struct json_object* value;
   if (json_object_object_get_ex(root, "baudrate", &value))
      valid = valid && json_object_is_type(value, json_type_int) && json_object_get_int64(value) == 115200;
   if (json_object_object_get_ex(root, "timeout_seconds", &value)) {
      bool numeric = json_object_is_type(value, json_type_int) || json_object_is_type(value, json_type_double);
      config->timeout = json_object_get_double(value);
      valid = valid && numeric && isfinite(config->timeout) && config->timeout > 0;
   }
   json_object_put(root);
   if (!valid)
      return CableConfig_error(error, size, "Cable: invalid nvpowermon config fields");
   return true;
}

static bool CableConfig_read(const char* directory, const char* name, char* out, size_t size) {
   char path[PATH_MAX];
   int length = xSnprintf(path, sizeof(path), "%s/%s", directory, name);
   if (length < 0 || (size_t)length >= sizeof(path))
      return false;
   FILE* file = fopen(path, "r");
   if (!file)
      return false;
   bool ok = fgets(out, size, file) != NULL;
   fclose(file);
   if (ok)
      out[strcspn(out, "\r\n")] = '\0';
   return ok;
}

static bool CableConfig_usb(const char* sysRoot, const char* tty, char* serial, char* vid, char* pid) {
   char path[PATH_MAX], node[PATH_MAX];
   int length = xSnprintf(path, sizeof(path), "%s/%s/device", sysRoot, tty);
   if (length < 0 || (size_t)length >= sizeof(path) || !realpath(path, node))
      return false;
   while (*node) {
      if (CableConfig_read(node, "idVendor", vid, 16) && CableConfig_read(node, "idProduct", pid, 16)) {
         serial[0] = '\0';
         CableConfig_read(node, "serial", serial, 256);
         return CableConfig_hex(vid) && CableConfig_hex(pid);
      }
      char* slash = strrchr(node, '/');
      if (!slash)
         break;
      *slash = '\0';
   }
   return false;
}

bool CableConfig_resolve(const CableConfig* config, const char* sysRoot, const char* devRoot,
                         char* port, size_t portSize, char* error, size_t errorSize) {
   if (!*config->serial && !*config->vid && !*config->pid) {
      xSnprintf(port, portSize, "%s", config->port);
      return true;
   }
   DIR* directory = opendir(sysRoot);
   if (!directory)
      return CableConfig_error(error, errorSize, "Cable: cannot enumerate USB serial devices");
   char configured[PATH_MAX];
   bool hasConfigured = !*config->serial && realpath(config->port, configured);
   unsigned int matches = 0;
   struct dirent* entry;
   while ((entry = readdir(directory))) {
      if (!String_startsWith(entry->d_name, "ttyACM") && !String_startsWith(entry->d_name, "ttyUSB"))
         continue;
      char serial[256], vid[16], pid[16], candidate[PATH_MAX];
      if (!CableConfig_usb(sysRoot, entry->d_name, serial, vid, pid))
         continue;
      if ((*config->vid && !String_eq(config->vid, vid)) || (*config->pid && !String_eq(config->pid, pid)))
         continue;
      int length = xSnprintf(candidate, sizeof(candidate), "%s/%s", devRoot, entry->d_name);
      if (length < 0 || (size_t)length >= sizeof(candidate))
         continue;
      if (*config->serial) {
         if (!String_eq(config->serial, serial))
            continue;
      } else {
         char resolved[PATH_MAX];
         if (!hasConfigured || !realpath(candidate, resolved) || !String_eq(configured, resolved))
            continue;
      }
      xSnprintf(port, portSize, "%s", *config->serial ? candidate : config->port);
      matches++;
   }
   closedir(directory);
   if (matches != 1)
      return CableConfig_error(error, errorSize, matches ? "Cable: USB serial matches multiple ports" : "Cable: configured USB device not found (no fallback)");
   return true;
}
