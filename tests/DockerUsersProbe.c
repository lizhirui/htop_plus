/* Exercise the real Docker worker without touching a real Docker daemon. */
#include "config.h" // IWYU pragma: keep

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "CRT.h"
#include "linux/DockerUsers.h"

void CRT_done(void) {
}

int main(int argc, char** argv) {
   if (argc < 5)
      return 2;
   DockerUsers* users = DockerUsers_new(argv[1]);
   int loops = argc > 5 ? atoi(argv[5]) : 20;
   for (int i = 0; i < loops; i++) {
      char label[512];
      bool container = DockerUsers_format(users, argv[2], strtoul(argv[3], NULL, 10), argv[4], label, sizeof(label));
      printf("%s\n", container ? label : "HOST");
      fflush(stdout);
      struct timespec pause = { .tv_nsec = 50000000 };
      nanosleep(&pause, NULL);
   }
   DockerUsers_delete(users);
   return 0;
}
