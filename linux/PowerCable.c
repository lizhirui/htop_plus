/*
htop - linux/PowerCable.c
Released under the GNU GPLv2+, see the COPYING file.
Protocol: WINGSTUDIO 12V-2X6 Ti firmware, ASCII 115200 8N1.
*/

#include "config.h" // IWYU pragma: keep

#include "linux/PowerCable.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include "XUtils.h"
#include "linux/CableConfig.h"


static PowerCableReading reading;
static int serialFd = -1;
static enum { CABLE_CLOSED, CABLE_ID, CABLE_SENSOR, CABLE_IDLE } state;
static char buffer[1024];
static size_t buffered;
static double deadline;
static double nextQuery;
static double queryTimeout = 4.0;

static double PowerCable_now(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec / 1e9;
}

void PowerCable_done(void) {
   if (serialFd >= 0)
      close(serialFd);
   serialFd = -1;
   state = CABLE_CLOSED;
   buffered = 0;
   reading.valid = false;
}

static void PowerCable_fail(const char* message, double now) {
   PowerCable_done();
   xSnprintf(reading.message, sizeof(reading.message), "%s", message);
   nextQuery = now + 5.0;
}

/* Check existing visible readers too: TIOCEXCL only prevents future opens. */
static bool PowerCable_inUse(const char* port) {
   char target[PATH_MAX];
   if (!realpath(port, target))
      return false;
   glob_t paths = {0};
   bool used = false;
   if (glob("/proc/[0-9]*/fd/*", 0, NULL, &paths) == 0) {
      for (size_t i = 0; i < paths.gl_pathc; i++) {
         char link[PATH_MAX];
         ssize_t len = readlink(paths.gl_pathv[i], link, sizeof(link) - 1);
         if (len < 0)
            continue;
         link[len] = '\0';
         if (String_eq(link, target)) {
            used = true;
            break;
         }
      }
   }
   globfree(&paths);
   return used;
}

static bool PowerCable_send(bool identity, double now) {
   const char* command = identity ? "*IDN?\n" : "SENSORDATA?\n";
   size_t length = strlen(command);
   if (write(serialFd, command, length) != (ssize_t)length) {
      PowerCable_fail("Cable: serial write failed", now);
      return false;
   }
   state = identity ? CABLE_ID : CABLE_SENSOR;
   deadline = now + queryTimeout;
   return true;
}

static void PowerCable_open(double now) {
   const char* port = getenv("HTOP_POWER_CABLE_PORT");
   char selected[PATH_MAX];
   if (port && String_eq(port, "off")) {
      PowerCable_fail("Cable disabled (HTOP_POWER_CABLE_PORT=off)", now);
      return;
   }
   if (!port || !*port) {
      const char* path = getenv("HTOP_POWER_CABLE_CONFIG");
      if (!path || !*path)
         path = "/etc/nvpowermon/config.json";
      CableConfig config;
      char error[160];
      if (!CableConfig_load(path, &config, error, sizeof(error)) ||
          !CableConfig_resolve(&config, "/sys/class/tty", "/dev", selected, sizeof(selected), error, sizeof(error))) {
         PowerCable_fail(error, now);
         return;
      }
      queryTimeout = config.timeout;
      port = selected;
   } else {
      queryTimeout = 4.0;
   }
   if (!String_startsWith(port, "/dev/")) {
      PowerCable_fail("Cable port must be an absolute /dev/ path", now);
      return;
   }
   if (PowerCable_inUse(port)) {
      PowerCable_fail("Cable busy: close other serial monitors", now);
      return;
   }
   serialFd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
   if (serialFd < 0) {
      PowerCable_fail(errno == EACCES ? "Cable permission denied (dialout group)" : "Cable unavailable / busy", now);
      return;
   }
   struct stat st;
   struct termios attrs;
   if (fstat(serialFd, &st) != 0 || !S_ISCHR(st.st_mode) ||
       flock(serialFd, LOCK_EX | LOCK_NB) != 0 || ioctl(serialFd, TIOCEXCL) != 0 ||
       tcgetattr(serialFd, &attrs) != 0) {
      PowerCable_fail("Cable: exclusive serial access failed", now);
      return;
   }
   attrs.c_iflag = attrs.c_oflag = attrs.c_lflag = 0;
   /* Do not toggle DTR/RTS, send BREAK, or request a line drop on close. */
   attrs.c_cflag = CS8 | CREAD | CLOCAL;
   attrs.c_cc[VMIN] = attrs.c_cc[VTIME] = 0;
   cfsetispeed(&attrs, B115200);
   cfsetospeed(&attrs, B115200);
   if (tcsetattr(serialFd, TCSANOW, &attrs) != 0 || tcflush(serialFd, TCIFLUSH) != 0) {
      PowerCable_fail("Cable: serial setup failed", now);
      return;
   }
   xSnprintf(reading.message, sizeof(reading.message), "Cable: verifying device ID...");
   PowerCable_send(true, now);
}

static bool PowerCable_parse(const char* line, double now) {
   double values[15];
   for (size_t i = 0; i < 15; i++) {
      while (isspace((unsigned char)*line))
         line++;
      /* strtod accepts nan/inf, hexadecimal and empty fields: reject them. */
      if (!isdigit((unsigned char)*line) && *line != '-' && *line != '+' && *line != '.')
         return false;
      char* end;
      errno = 0;
      values[i] = strtod(line, &end);
      if (end == line || errno != 0 || !isfinite(values[i]))
         return false;
      for (const char* c = line; c < end; c++)
         if (!isdigit((unsigned char)*c) && *c != '+' && *c != '-' && *c != '.' && *c != 'e' && *c != 'E')
            return false;
      line = end;
      while (isspace((unsigned char)*line))
         line++;
      if (i < 14) {
         if (*line++ != ',')
            return false;
      } else if (*line != '\0') {
         return false;
      }
   }
   for (size_t i = 0; i < 6; i++) {
      reading.voltage[i] = values[2 * i];
      reading.current[i] = values[2 * i + 1];
   }
   reading.power = values[12];
   reading.connectorTemperature = values[13];
   reading.controllerTemperature = values[14];
   reading.updated = now;
   reading.valid = true;
   reading.message[0] = '\0';
   return true;
}

static bool PowerCable_line(double now) {
   buffer[buffered] = '\0';
   if (!buffered)
      return true;
   if (state == CABLE_ID) {
      if (!String_startsWith(buffer, "WINGSTUDIO 12V-2X6 Ti V")) {
         PowerCable_fail("Cable: wrong device ID", now);
         return false;
      }
      xSnprintf(reading.message, sizeof(reading.message), "Cable: reading sensors...");
      return PowerCable_send(false, now);
   }
   if (state != CABLE_SENSOR || !PowerCable_parse(buffer, now)) {
      PowerCable_fail("Cable: invalid sensor response", now);
      return false;
   }
   state = CABLE_IDLE;
   nextQuery = now + 2.0;
   return true;
}

const PowerCableReading* PowerCable_get(void) {
   double now = PowerCable_now();
   if (reading.valid && now - reading.updated > 10.0) {
      PowerCable_fail("Cable: stale data / no recent sample", now);
      return &reading;
   }
   if (state == CABLE_CLOSED) {
      if (now >= nextQuery)
         PowerCable_open(now);
      return &reading;
   }
   if (state == CABLE_IDLE && now >= nextQuery && !PowerCable_send(false, now))
      return &reading;

   struct pollfd pfd = { .fd = serialFd, .events = POLLIN };
   if (poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))) {
      PowerCable_fail("Cable disconnected / serial hangup", now);
      return &reading;
   }

   /* A bounded drain prevents noisy or faulty devices from monopolizing UI. */
   char incoming[1024];
   ssize_t size = read(serialFd, incoming, sizeof(incoming));
   if (size < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      PowerCable_fail("Cable disconnected / serial read failed", now);
      return &reading;
   }
   for (ssize_t i = 0; i < size; i++) {
      unsigned char c = incoming[i];
      if (c == '\r')
         continue;
      if (c == '\n') {
         if (!PowerCable_line(now))
            return &reading;
         buffered = 0;
      } else if (c < 32 || c > 126 || buffered >= sizeof(buffer) - 1) {
         PowerCable_fail("Cable: malformed / oversized response", now);
         return &reading;
      } else {
         buffer[buffered++] = c;
      }
   }
   if ((state == CABLE_ID || state == CABLE_SENSOR) && now >= deadline) {
      char message[80];
      xSnprintf(message, sizeof(message), "Cable: query timed out (%gs)", queryTimeout);
      PowerCable_fail(message, now);
   }
   return &reading;
}
