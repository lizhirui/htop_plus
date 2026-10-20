/*
htop - linux/PowerCable.c
Released under the GNU GPLv2+, see the COPYING file.
Read-only subscriber to the nvpowermon socket protocol v1.
*/

#include "config.h" // IWYU pragma: keep

#include "linux/PowerCable.h"

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <json-c/json.h>

#include "XUtils.h"


static PowerCableReading reading;
static int socketFd = -1;
static bool connecting;
/* The protocol limit includes LF; reserve one byte for the terminating NUL. */
static char buffer[65536];
static size_t buffered;
static int64_t lastSequence;
static double deadline;
static double nextConnect;

static double PowerCable_now(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec / 1e9;
}

void PowerCable_done(void) {
   if (socketFd >= 0)
      close(socketFd);
   socketFd = -1;
   connecting = false;
   buffered = 0;
   lastSequence = 0;
   nextConnect = 0;
   reading.valid = false;
}

static void PowerCable_fail(const char* message, double now) {
   PowerCable_done();
   xSnprintf(reading.message, sizeof(reading.message), "%s", message);
   nextConnect = now + 5.0;
}

static void PowerCable_open(double now) {
   const char* path = getenv("HTOP_POWER_CABLE_SOCKET");
   if (!path || !*path)
      path = getenv("NVPOWERMON_SOCKET");
   if (!path || !*path)
      path = "/run/nvpowermon/status.sock";
   const char* oldPort = getenv("HTOP_POWER_CABLE_PORT");
   if (String_eq(path, "off") || (oldPort && String_eq(oldPort, "off"))) {
      PowerCable_fail("Cable disabled", now);
      return;
   }
   if ((oldPort && *oldPort) || getenv("HTOP_POWER_CABLE_CONFIG")) {
      PowerCable_fail("Cable: configure the device in nvpowermon; use HTOP_POWER_CABLE_SOCKET", now);
      return;
   }
   struct sockaddr_un address = { .sun_family = AF_UNIX };
   if (*path != '/' || strlen(path) >= sizeof(address.sun_path)) {
      PowerCable_fail("Cable: invalid nvpowermon socket path", now);
      return;
   }
   xSnprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
   socketFd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
   if (socketFd < 0) {
      PowerCable_fail("Cable: cannot create nvpowermon socket", now);
      return;
   }
   if (connect(socketFd, (struct sockaddr*)&address, sizeof(address)) < 0) {
      if (errno != EINPROGRESS) {
         PowerCable_fail(errno == EACCES ? "Cable: nvpowermon socket permission denied" :
                         "Cable: service unavailable; run sudo nvpowermon --start", now);
         return;
      }
      connecting = true;
   }
   deadline = now + 15.0;
   xSnprintf(reading.message, sizeof(reading.message), "Cable: waiting for nvpowermon...");
}

static struct json_object* PowerCable_field(struct json_object* root, const char* key, enum json_type type) {
   struct json_object* value;
   if (!json_object_is_type(root, json_type_object) ||
       !json_object_object_get_ex(root, key, &value) || !json_object_is_type(value, type))
      return NULL;
   return value;
}

static const char* PowerCable_string(struct json_object* root, const char* key) {
   struct json_object* value = PowerCable_field(root, key, json_type_string);
   if (!value)
      return NULL;
   const char* text = json_object_get_string(value);
   return strlen(text) == (size_t)json_object_get_string_len(value) ? text : NULL;
}

static bool PowerCable_number(struct json_object* root, const char* key, double* out) {
   struct json_object* value;
   if (!json_object_is_type(root, json_type_object) || !json_object_object_get_ex(root, key, &value) ||
       (!json_object_is_type(value, json_type_int) && !json_object_is_type(value, json_type_double)))
      return false;
   *out = json_object_get_double(value);
   return isfinite(*out);
}

/* Older json-c versions do not validate UTF-8. Reject invalid byte sequences
 * before parsing, including overlong encodings, surrogates and embedded NUL. */
static bool PowerCable_utf8(const unsigned char* text, size_t size) {
   for (size_t i = 0; i < size; ) {
      unsigned int c = text[i++];
      if (c == 0)
         return false;
      if (c < 0x80)
         continue;
      unsigned int count, minimum;
      if (c >= 0xc2 && c <= 0xdf) {
         count = 1;
         minimum = 0x80;
         c &= 0x1f;
      } else if (c >= 0xe0 && c <= 0xef) {
         count = 2;
         minimum = 0x800;
         c &= 0x0f;
      } else if (c >= 0xf0 && c <= 0xf4) {
         count = 3;
         minimum = 0x10000;
         c &= 0x07;
      } else {
         return false;
      }
      if (count > size - i)
         return false;
      while (count--) {
         unsigned int next = text[i++];
         if ((next & 0xc0) != 0x80)
            return false;
         c = (c << 6) | (next & 0x3f);
      }
      if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
         return false;
   }
   return true;
}

static const char* PowerCable_snapshot(struct json_object* root, double now) {
   struct json_object* version = PowerCable_field(root, "schema_version", json_type_int);
   if (!version || json_object_get_int64(version) != 1)
      return "Cable: unsupported nvpowermon protocol";
   struct json_object* sequence = PowerCable_field(root, "sequence", json_type_int);
   struct json_object* valid = PowerCable_field(root, "valid", json_type_boolean);
   const char* state = PowerCable_string(root, "state");
   if (!sequence || json_object_get_int64(sequence) < 1 || !valid || !state ||
       (!String_eq(state, "ok") && !String_eq(state, "starting") && !String_eq(state, "error")) ||
       (bool)json_object_get_boolean(valid) != String_eq(state, "ok"))
      return "Cable: invalid nvpowermon metadata";

   PowerCableReading sample = {0};
   if (json_object_get_boolean(valid)) {
      struct json_object* device = PowerCable_field(root, "device", json_type_object);
      struct json_object* status = PowerCable_field(root, "status", json_type_object);
      struct json_object* config = PowerCable_field(root, "device_config", json_type_object);
      struct json_object* channels = PowerCable_field(status, "channels", json_type_array);
      const char* id = PowerCable_string(root, "id");
      if (!PowerCable_string(root, "time") || !PowerCable_string(device, "port") ||
          !id || !String_startsWith(id, "WINGSTUDIO 12V-2X6 Ti V") ||
          !channels || json_object_array_length(channels) != 6 ||
          !PowerCable_number(root, "sample_monotonic_s", &sample.updated))
         return "Cable: invalid nvpowermon snapshot";
      if (now - sample.updated < 0 || now - sample.updated > 10.0)
         return "Cable: stale data / invalid sample time";
      for (size_t i = 0; i < 6; i++) {
         struct json_object* channel = json_object_array_get_idx(channels, i);
         const char* name = PowerCable_string(channel, "channel");
         if (!name || name[0] != (char)('A' + i) || name[1] != '\0' ||
             !PowerCable_number(channel, "voltage_v", &sample.voltage[i]) ||
             !PowerCable_number(channel, "current_a", &sample.current[i]))
            return "Cable: invalid nvpowermon channel";
      }
      if (!PowerCable_number(status, "total_power_w", &sample.power) ||
          !PowerCable_number(status, "cable_temperature_c", &sample.connectorTemperature) ||
          !PowerCable_number(status, "board_temperature_c", &sample.controllerTemperature))
         return "Cable: invalid nvpowermon sensors";
      static const char* const fields[] = {
         "screen_rotation", "shunt_resistance_milliohm", "current_correction_rate",
         "fan_control_type", "fan_on_temperature_c", "alarm_temperature_c",
         "alarm_current_per_channel_a", "alarm_timeout_s", "protection_current_per_channel_a",
         "protection_temperature_c",
      };
      for (size_t i = 0; i < ARRAYSIZE(fields); i++) {
         double value;
         if (!PowerCable_number(config, fields[i], &value) ||
             (String_eq(fields[i], "fan_control_type") && (value < 0 || value > 2 || floor(value) < value)))
            return "Cable: invalid nvpowermon device configuration";
      }
      sample.valid = true;
   } else if (String_eq(state, "error")) {
      const char* error = PowerCable_string(root, "error");
      if (!error)
         return "Cable: invalid nvpowermon error";
      size_t size = strnlen(error, sizeof(sample.message) - 8);
      /* Truncate at a UTF-8 boundary and keep control characters off screen. */
      while (size && ((unsigned char)error[size] & 0xc0) == 0x80)
         size--;
      xSnprintf(sample.message, sizeof(sample.message), "Cable: %.*s", (int)size, error);
      for (char* c = sample.message; *c; c++)
         if ((unsigned char)*c < 32 || *c == 127)
            *c = ' ';
   } else {
      xSnprintf(sample.message, sizeof(sample.message), "Cable: nvpowermon starting...");
   }
   /* Sequence numbers are scoped to this connection, not the service lifetime. */
   int64_t number = json_object_get_int64(sequence);
   if (number > lastSequence) {
      lastSequence = number;
      reading = sample;
      deadline = now + (String_eq(state, "starting") ? 15.0 : 10.0);
   }
   return NULL;
}

static bool PowerCable_line(void) {
   double now = PowerCable_now();
   const char* error = "Cable: invalid nvpowermon JSON";
   if (PowerCable_utf8((const unsigned char*)buffer, buffered)) {
      buffer[buffered] = '\0';
      struct json_tokener* tokener = json_tokener_new();
      if (tokener) {
         json_tokener_set_flags(tokener, JSON_TOKENER_STRICT);
         struct json_object* root = json_tokener_parse_ex(tokener, buffer, buffered + 1);
         if (json_tokener_get_error(tokener) == json_tokener_success &&
             json_object_is_type(root, json_type_object))
            error = PowerCable_snapshot(root, now);
         json_object_put(root);
         json_tokener_free(tokener);
      }
   }
   if (error)
      PowerCable_fail(error, now);
   return !error;
}

const PowerCableReading* PowerCable_get(void) {
   double now = PowerCable_now();
   if (socketFd < 0 && now >= nextConnect)
      PowerCable_open(now);
   if (socketFd < 0)
      return &reading;

   if (connecting) {
      struct pollfd pfd = { .fd = socketFd, .events = POLLOUT };
      if (poll(&pfd, 1, 0) > 0) {
         int error = 0;
         socklen_t size = sizeof(error);
         if (getsockopt(socketFd, SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error ||
             !(pfd.revents & POLLOUT)) {
            PowerCable_fail("Cable: nvpowermon connection failed", now);
            return &reading;
         }
         connecting = false;
      }
   }
   /* Bound work per refresh even if a faulty peer continuously sends data. */
   for (unsigned int batch = 0; !connecting && batch < 16; batch++) {
      char incoming[4096];
      ssize_t size = recv(socketFd, incoming, sizeof(incoming), 0);
      if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
         break;
      if (size <= 0) {
         PowerCable_fail("Cable: nvpowermon disconnected", now);
         return &reading;
      }
      for (ssize_t i = 0; i < size; i++) {
         if (incoming[i] == '\n') {
            if (!PowerCable_line())
               return &reading;
            buffered = 0;
         } else if (buffered >= sizeof(buffer) - 1) {
            PowerCable_fail("Cable: oversized nvpowermon frame", now);
            return &reading;
         } else {
            buffer[buffered++] = incoming[i];
         }
      }
   }
   now = PowerCable_now();
   if (reading.valid && now - reading.updated > 10.0) {
      PowerCable_fail("Cable: stale data / no recent sample", now);
   } else if (now >= deadline) {
      PowerCable_fail("Cable: nvpowermon subscription timed out", now);
   } else {
      struct pollfd pfd = { .fd = socketFd, .events = POLLIN };
      if (poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)))
         PowerCable_fail("Cable: nvpowermon disconnected", now);
   }
   return &reading;
}
