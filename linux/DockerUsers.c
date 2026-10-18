/* htop - released under the GNU GPLv2+, see COPYING. */

#include "config.h" // IWYU pragma: keep

#include "linux/DockerUsers.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>
#include <json-c/json.h>

#include "XUtils.h"


/* All Docker I/O stays in one worker. The UI only reads bounded snapshots.
 * Requests are restricted to GET containers/json and GET .../archive?path=/etc/passwd.
 * No shell, docker exec, namespace entry, or container mutations are involved. */
typedef struct DockerAccount_ {
   uint32_t uid;
   char name[256];
   struct DockerAccount_* next;
} DockerAccount;

typedef struct DockerContainer_ {
   char id[65];
   char name[256];
   DockerAccount* accounts;
   struct DockerContainer_* next;
} DockerContainer;

struct DockerUsers_ {
   pthread_t thread;
   pthread_mutex_t mutex;
   pthread_cond_t wake;
   bool started;
   bool curlInitialized;
   bool stop;
   char socket[PATH_MAX];
   DockerContainer* containers;
};

typedef struct DockerResponse_ {
   char* data;
   size_t size;
} DockerResponse;

static bool DockerUsers_stopping(DockerUsers* this) {
   pthread_mutex_lock(&this->mutex);
   bool stop = this->stop;
   pthread_mutex_unlock(&this->mutex);
   return stop;
}

static int DockerUsers_progress(void* opaque, curl_off_t total ATTR_UNUSED, curl_off_t now ATTR_UNUSED,
                                curl_off_t uploadTotal ATTR_UNUSED, curl_off_t uploadNow ATTR_UNUSED) {
   return DockerUsers_stopping(opaque);
}

static size_t DockerUsers_receive(char* data, size_t size, size_t count, void* opaque) {
   DockerResponse* response = opaque;
   if (size && count > SIZE_MAX / size)
      return 0;
   size_t length = size * count;
   if (length > 2 * 1024 * 1024 - response->size)
      return 0;
   response->data = xRealloc(response->data, response->size + length + 1);
   memcpy(response->data + response->size, data, length);
   response->size += length;
   response->data[response->size] = '\0';
   return length;
}

static bool DockerUsers_get(DockerUsers* this, const char* path, DockerResponse* response) {
   CURL* curl = curl_easy_init();
   if (!curl)
      return false;
   char url[256];
   xSnprintf(url, sizeof(url), "http://localhost%s", path);
   curl_easy_setopt(curl, CURLOPT_URL, url);
   curl_easy_setopt(curl, CURLOPT_UNIX_SOCKET_PATH, this->socket);
   curl_easy_setopt(curl, CURLOPT_PROXY, "");
   curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 250L);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 750L);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, DockerUsers_receive);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
   curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, DockerUsers_progress);
   curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
   curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
   CURLcode result = curl_easy_perform(curl);
   long status = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
   curl_easy_cleanup(curl);
   return result == CURLE_OK && status == 200;
}

static void DockerUsers_freeContainers(DockerContainer* container) {
   while (container) {
      DockerContainer* next = container->next;
      while (container->accounts) {
         DockerAccount* account = container->accounts;
         container->accounts = account->next;
         free(account);
      }
      free(container);
      container = next;
   }
}

static bool DockerUsers_id(const char* text, char* id) {
   for (size_t i = 0; i < 64; i++) {
      if (!isxdigit((unsigned char)text[i]))
         return false;
      id[i] = tolower((unsigned char)text[i]);
   }
   id[64] = '\0';
   return true;
}

static bool DockerUsers_cgroupId(const char* cgroup, char* id) {
   if (!cgroup)
      return false;
   for (const char* at = cgroup; *at; at++) {
      if (*at != '/')
         continue;
      const char* value = NULL;
      bool scope = false;
      if (String_startsWith(at, "/docker/")) {
         value = at + 8;
      } else if (String_startsWith(at, "/docker-")) {
         value = at + 8;
         scope = true;
      }
      if (!value || strlen(value) < 64 || !DockerUsers_id(value, id))
         continue;
      const char* end = value + 64;
      if (scope) {
         if (!String_startsWith(end, ".scope"))
            continue;
         end += 6;
      }
      if (!*end || *end == '/' || *end == ';' || *end == '\n')
         return true;
   }
   return false;
}

static DockerAccount* DockerUsers_passwd(char* text) {
   DockerAccount* accounts = NULL;
   char* save;
   unsigned int count = 0;
   for (char* line = strtok_r(text, "\n", &save); line && count < 4096; line = strtok_r(NULL, "\n", &save)) {
      char* password = strchr(line, ':');
      if (!password || password == line || password - line >= 256)
         continue;
      char* uid = strchr(password + 1, ':');
      if (!uid || !isdigit((unsigned char)uid[1]))
         continue;
      char* end;
      errno = 0;
      unsigned long value = strtoul(uid + 1, &end, 10);
      if (errno || *end != ':' || value > UINT32_MAX)
         continue;
      *password = '\0';
      DockerAccount* account = xCalloc(1, sizeof(DockerAccount));
      account->uid = value;
      xSnprintf(account->name, sizeof(account->name), "%s", line);
      for (char* c = account->name; *c; c++)
         if ((unsigned char)*c < 32 || *c == 127)
            *c = '?';
      account->next = accounts;
      accounts = account;
      count++;
   }
   return accounts;
}

static DockerAccount* DockerUsers_archive(const DockerResponse* response) {
   for (size_t at = 0; at + 512 <= response->size; ) {
      const char* header = response->data + at;
      if (!header[0])
         break;
      size_t length = 0;
      for (size_t i = 124; i < 136; i++) {
         unsigned char c = header[i];
         if (c == ' ' || c == '\0')
            continue;
         if (c < '0' || c > '7' || length > 2 * 1024 * 1024)
            return NULL;
         length = length * 8 + c - '0';
      }
      at += 512;
      if (length > response->size - at)
         return NULL;
      if ((header[156] == '0' || header[156] == '\0') &&
          (String_eq(header, "passwd") || String_eq(header, "etc/passwd"))) {
         char* text = xStrndup(response->data + at, length);
         DockerAccount* accounts = DockerUsers_passwd(text);
         free(text);
         return accounts;
      }
      at += (length + 511) / 512 * 512;
   }
   return NULL;
}

static DockerContainer* DockerUsers_snapshot(DockerUsers* this) {
   DockerResponse response = {0};
   struct json_object* root = NULL;
   if (DockerUsers_get(this, "/containers/json", &response) && response.data)
      root = json_tokener_parse(response.data);
   free(response.data);
   if (!root || !json_object_is_type(root, json_type_array)) {
      json_object_put(root);
      return NULL;
   }
   DockerContainer* containers = NULL;
   size_t count = json_object_array_length(root);
   for (size_t i = 0; i < count && i < 256 && !DockerUsers_stopping(this); i++) {
      struct json_object* item = json_object_array_get_idx(root, i);
      struct json_object *identity, *names;
      if (!json_object_is_type(item, json_type_object) ||
          !json_object_object_get_ex(item, "Id", &identity) || !json_object_is_type(identity, json_type_string) ||
          !json_object_object_get_ex(item, "Names", &names) || !json_object_is_type(names, json_type_array) ||
          json_object_array_length(names) == 0)
         continue;
      const char* id = json_object_get_string(identity);
      if (strlen(id) != 64)
         continue;
      struct json_object* first = json_object_array_get_idx(names, 0);
      if (!json_object_is_type(first, json_type_string))
         continue;
      const char* name = json_object_get_string(first);
      while (*name == '/')
         name++;
      if (!*name || strlen(name) >= 256)
         continue;
      DockerContainer* container = xCalloc(1, sizeof(DockerContainer));
      if (!DockerUsers_id(id, container->id)) {
         free(container);
         continue;
      }
      xSnprintf(container->name, sizeof(container->name), "%s", name);
      for (char* c = container->name; *c; c++)
         if (!isalnum((unsigned char)*c) && *c != '_' && *c != '-' && *c != '.')
            *c = '_';
      char path[128];
      xSnprintf(path, sizeof(path), "/containers/%s/archive?path=/etc/passwd", container->id);
      response = (DockerResponse) {0};
      if (DockerUsers_get(this, path, &response))
         container->accounts = DockerUsers_archive(&response);
      free(response.data);
      container->next = containers;
      containers = container;
   }
   json_object_put(root);
   return containers;
}

static void* DockerUsers_run(void* opaque) {
   DockerUsers* this = opaque;
   while (!DockerUsers_stopping(this)) {
      DockerContainer* snapshot = DockerUsers_snapshot(this);
      pthread_mutex_lock(&this->mutex);
      DockerContainer* previous = this->containers;
      this->containers = snapshot;
      pthread_mutex_unlock(&this->mutex);
      DockerUsers_freeContainers(previous);
      struct timespec until;
      clock_gettime(CLOCK_REALTIME, &until);
      until.tv_sec += 5;
      pthread_mutex_lock(&this->mutex);
      if (!this->stop)
         pthread_cond_timedwait(&this->wake, &this->mutex, &until);
      pthread_mutex_unlock(&this->mutex);
   }
   return NULL;
}

DockerUsers* DockerUsers_new(const char* socketPath) {
   DockerUsers* this = xCalloc(1, sizeof(DockerUsers));
   pthread_mutex_init(&this->mutex, NULL);
   pthread_cond_init(&this->wake, NULL);
   if (!socketPath) {
      const char* host = getenv("DOCKER_HOST");
      if (host && *host) {
         socketPath = String_startsWith(host, "unix://") ? host + 7 : "";
      } else {
         const char* runtime = getenv("XDG_RUNTIME_DIR");
         if (runtime)
            xSnprintf(this->socket, sizeof(this->socket), "%s/docker.sock", runtime);
         if (!*this->socket || access(this->socket, R_OK | W_OK) != 0)
            xSnprintf(this->socket, sizeof(this->socket), "/var/run/docker.sock");
      }
   }
   if (socketPath)
      xSnprintf(this->socket, sizeof(this->socket), "%s", socketPath);
   this->curlInitialized = *this->socket && curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
   if (this->curlInitialized)
      this->started = pthread_create(&this->thread, NULL, DockerUsers_run, this) == 0;
   return this;
}

void DockerUsers_delete(DockerUsers* this) {
   if (!this)
      return;
   pthread_mutex_lock(&this->mutex);
   this->stop = true;
   pthread_cond_signal(&this->wake);
   pthread_mutex_unlock(&this->mutex);
   if (this->started)
      pthread_join(this->thread, NULL);
   if (this->curlInitialized)
      curl_global_cleanup();
   DockerUsers_freeContainers(this->containers);
   pthread_cond_destroy(&this->wake);
   pthread_mutex_destroy(&this->mutex);
   free(this);
}

static bool DockerUsers_innerUid(const char* path, uid_t hostUid, uint32_t* uid) {
   FILE* file = fopen(path, "r");
   if (!file)
      return false;
   unsigned long long inside, outside, length;
   bool found = false;
   while (fscanf(file, "%llu %llu %llu", &inside, &outside, &length) == 3) {
      if (hostUid >= outside && hostUid - outside < length && inside + hostUid - outside <= UINT32_MAX) {
         *uid = inside + hostUid - outside;
         found = true;
         break;
      }
   }
   fclose(file);
   return found;
}

bool DockerUsers_format(DockerUsers* this, const char* cgroup, uid_t hostUid,
                        const char* uidMapPath, char* out, size_t size) {
   char id[65];
   if (!DockerUsers_cgroupId(cgroup, id))
      return false;
   uint32_t uid;
   bool mapped = DockerUsers_innerUid(uidMapPath, hostUid, &uid);
   char account[256], name[256];
   if (mapped)
      xSnprintf(account, sizeof(account), "%u", uid);
   else
      xSnprintf(account, sizeof(account), "uid:%u", (unsigned int)hostUid);
   xSnprintf(name, sizeof(name), "%.12s", id);
   pthread_mutex_lock(&this->mutex);
   for (const DockerContainer* container = this->containers; container; container = container->next) {
      if (!String_eq(container->id, id))
         continue;
      xSnprintf(name, sizeof(name), "%s", container->name);
      if (mapped) {
         for (const DockerAccount* user = container->accounts; user; user = user->next) {
            if (user->uid == uid) {
               xSnprintf(account, sizeof(account), "%s", user->name);
               break;
            }
         }
      }
      break;
   }
   pthread_mutex_unlock(&this->mutex);
   xSnprintf(out, size, "[%s]%s", name, account);
   return true;
}
