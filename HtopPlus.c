/*
htop_plus - native entry point and initial layout selection.
Released under the GNU GPLv2+, see the COPYING file.
*/

#include "config.h" // IWYU pragma: keep

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "CommandLine.h"
#include "XUtils.h"


const char* program = "htop_plus";

static char* HtopPlus_directory(const char* executable) {
   char path[PATH_MAX];
#ifdef __linux__
   ssize_t size = readlink("/proc/self/exe", path, sizeof(path) - 1);
   if (size > 0 && (size_t)size < sizeof(path) - 1) {
      path[size] = '\0';
   } else
#endif
   if (!realpath(executable, path)) {
      /* argv[0] may be a bare command name on platforms without /proc. */
      const char* search = getenv("PATH");
      if (!search)
         return NULL;
      bool found = false;
      char* paths = xStrdup(search);
      char* next = paths;
      char* dir;
      while ((dir = strsep(&next, ":"))) {
         char* candidate;
         xAsprintf(&candidate, "%s/%s", *dir ? dir : ".", executable);
         found = access(candidate, X_OK) == 0 && realpath(candidate, path);
         free(candidate);
         if (found)
            break;
      }
      free(paths);
      if (!found)
         return NULL;
   }
   char* slash = strrchr(path, '/');
   if (!slash)
      return NULL;
   slash[1] = '\0';
   return xStrdup(path);
}

static bool HtopPlus_copyLayout(const char* defaults, const char* config) {
   char* path = xStrdup(config);
   for (char* c = path + (path[0] == '/' ? 1 : 0); *c; c++) {
      if (*c != '/')
         continue;
      *c = '\0';
      int result = mkdir(path, 0700);
      *c = '/';
      if (result != 0 && errno != EEXIST) {
         free(path);
         return false;
      }
   }
   free(path);

   /* Preserve existing layouts, including symlinks, and create private files. */
   int target = open(config, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
   if (target < 0)
      return errno == EEXIST;
   int source = open(defaults, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
   struct stat st;
   bool ok = source >= 0 && fstat(source, &st) == 0 && S_ISREG(st.st_mode);
   char buffer[4096];
   while (ok) {
      ssize_t size = read(source, buffer, sizeof(buffer));
      if (size < 0 && errno == EINTR)
         continue;
      if (size == 0)
         break;
      if (size < 0 || full_write(target, buffer, size) != size)
         ok = false;
   }
   if (source >= 0)
      close(source);
   if (close(target) != 0)
      ok = false;
   if (!ok)
      unlink(config);
   return ok;
}

static bool HtopPlus_prepareLayout(const char* executable) {
   const char* override = getenv("HTOPRC");
   if (override && *override)
      return true;
   char* directory = HtopPlus_directory(executable);
   if (!directory)
      return false;
   char* defaults = String_cat(directory, "htoprc.example");
   char* config;
   if (access(defaults, R_OK) == 0) {
      config = String_cat(directory, "htoprc");
   } else {
      free(defaults);
      defaults = String_cat(directory, "../share/htop_plus/htoprc.example");
      const char* xdg = getenv("XDG_CONFIG_HOME");
      if (xdg && *xdg) {
         config = String_cat(xdg, "/htop_plus/htoprc");
      } else {
         const char* home = getenv("HOME");
         if (!home || !*home) {
            const struct passwd* user = getpwuid(getuid());
            home = user ? user->pw_dir : NULL;
         }
         if (!home) {
            free(defaults);
            free(directory);
            return false;
         }
         config = String_cat(home, "/.config/htop_plus/htoprc");
      }
   }
   bool ok = HtopPlus_copyLayout(defaults, config) && setenv("HTOPRC", config, 1) == 0;
   free(config);
   free(defaults);
   free(directory);
   return ok;
}

int main(int argc, char** argv) {
   if (!HtopPlus_prepareLayout(argv[0])) {
      fprintf(stderr, "htop_plus: unable to prepare the layout; check configuration and installation paths\n");
      return EXIT_FAILURE;
   }
   return CommandLine_run(argc, argv);
}
