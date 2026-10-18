/* Read-only diagnostic and test driver for the real collection modules. */
#include "config.h" // IWYU pragma: keep

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "CRT.h"
#include "linux/NvidiaTelemetry.h"
#include "linux/PowerCable.h"


/* XUtils' out-of-memory handler needs this; the probe does not use curses. */
void CRT_done(void) {
}

int main(int argc, char** argv) {
   int loops = argc > 1 ? atoi(argv[1]) : 1;
   bool cable = argc > 2 && strcmp(argv[2], "cable") == 0;
   for (int n = 0; n < loops; n++) {
      if (cable) {
         const PowerCableReading* r = PowerCable_get();
         printf("cable valid=%d power=%.2f connector=%.2f controller=%.2f message=%s\n",
            r->valid, r->power, r->connectorTemperature, r->controllerTemperature, r->message);
         if (r->valid)
            for (int i = 0; i < 6; i++)
               printf("channel %c %.2fV %.2fA\n", 'A' + i, r->voltage[i], r->current[i]);
      } else {
         const NvidiaTelemetry* t = NvidiaTelemetry_get();
         printf("gpus=%u message=%s\n", t->count, t->message);
         for (unsigned int i = 0; i < t->count; i++) {
            const NvidiaGPU* g = &t->gpus[i];
            printf("gpu%u %s available=%d usage=%.1f memory=%.0f/%.0fMiB temp=%.1fC fan=%.0f%% rpm=%.0f power=%.2f/%.2fW\n",
               i, g->name, g->available, g->utilization, g->memoryUsedMiB,
               g->memoryTotalMiB, g->temperature, g->fanPercent, g->fanRPM, g->power, g->powerLimit);
         }
      }
      fflush(stdout);
      if (n + 1 < loops) {
         struct timespec pause = { .tv_nsec = 50000000 };
         nanosleep(&pause, NULL);
      }
   }
   PowerCable_done();
   NvidiaTelemetry_done();
   return 0;
}
