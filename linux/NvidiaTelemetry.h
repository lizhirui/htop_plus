#ifndef HEADER_NvidiaTelemetry
#define HEADER_NvidiaTelemetry
/*
htop - linux/NvidiaTelemetry.h
Released under the GNU GPLv2+, see the COPYING file.
*/

#include <stdbool.h>


#define NVIDIA_MAX_GPUS 32

typedef struct NvidiaGPU_ {
   char name[128];
   double utilization;
   double memoryUsedMiB;
   double memoryTotalMiB;
   double temperature;
   double fanPercent;
   double fanRPM;
   double power;
   double powerLimit;
   bool available;
} NvidiaGPU;

typedef struct NvidiaTelemetry_ {
   unsigned int count;
   NvidiaGPU gpus[NVIDIA_MAX_GPUS];
   char message[128];
} NvidiaTelemetry;

const NvidiaTelemetry* NvidiaTelemetry_get(void);
void NvidiaTelemetry_done(void);

#endif
