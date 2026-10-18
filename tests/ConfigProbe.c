/* Exercise configuration and sysfs discovery without opening a serial port. */
#include "config.h" // IWYU pragma: keep

#include <stdio.h>

#include "CRT.h"
#include "linux/CableConfig.h"

void CRT_done(void) {
}

int main(int argc, char** argv) {
   if (argc != 4)
      return 2;
   CableConfig config;
   char port[PATH_MAX], error[160];
   if (!CableConfig_load(argv[1], &config, error, sizeof(error)) ||
       !CableConfig_resolve(&config, argv[2], argv[3], port, sizeof(port), error, sizeof(error))) {
      puts(error);
      return 1;
   }
   printf("port=%s serial=%s vid=%s pid=%s timeout=%g\n", port, config.serial, config.vid, config.pid, config.timeout);
   return 0;
}
