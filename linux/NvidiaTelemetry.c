/*
htop - linux/NvidiaTelemetry.c
Released under the GNU GPLv2+, see the COPYING file.
*/

#include "config.h" // IWYU pragma: keep

#include "linux/NvidiaTelemetry.h"

#include <dlfcn.h>
#include <math.h>
#include <stddef.h>
#include <time.h>

#include "XUtils.h"


/* Stable public NVML ABI. No CUDA SDK or proprietary build dependency is needed.
 * Only query functions are resolved; never load clock/power/fan setters.
 * Layouts match NVIDIA's nvml.h (memory v1 and fan speed v1). */
typedef struct nvmlDevice_st* NvidiaDevice;
typedef struct { unsigned int gpu, memory; } NvidiaUtilization;
typedef struct { unsigned long long total, free, used; } NvidiaMemory;
typedef struct { unsigned int version, fan, speed; } NvidiaFanSpeed;

static struct {
   void* library;
   int (*init)(void);
   int (*shutdown)(void);
   int (*count)(unsigned int*);
   int (*handle)(unsigned int, NvidiaDevice*);
   int (*name)(NvidiaDevice, char*, unsigned int);
   int (*utilization)(NvidiaDevice, NvidiaUtilization*);
   int (*memory)(NvidiaDevice, NvidiaMemory*);
   int (*temperature)(NvidiaDevice, unsigned int, unsigned int*);
   int (*fan)(NvidiaDevice, unsigned int*);
   int (*rpm)(NvidiaDevice, NvidiaFanSpeed*);
   int (*power)(NvidiaDevice, unsigned int*);
   int (*limit)(NvidiaDevice, unsigned int*);
   bool initialized;
} nvml;

static NvidiaTelemetry telemetry;
static double nextPoll;

static double NvidiaTelemetry_now(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec / 1e9;
}

static bool NvidiaTelemetry_init(void) {
#ifdef BUILD_STATIC
   xSnprintf(telemetry.message, sizeof(telemetry.message), "NVML unavailable in static build");
   return false;
#else
   nvml.library = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
   if (!nvml.library) {
      xSnprintf(telemetry.message, sizeof(telemetry.message), "NVIDIA driver / NVML unavailable");
      return false;
   }

   *(void**)(&nvml.init) = dlsym(nvml.library, "nvmlInit_v2");
   *(void**)(&nvml.shutdown) = dlsym(nvml.library, "nvmlShutdown");
   *(void**)(&nvml.count) = dlsym(nvml.library, "nvmlDeviceGetCount_v2");
   *(void**)(&nvml.handle) = dlsym(nvml.library, "nvmlDeviceGetHandleByIndex_v2");
   *(void**)(&nvml.name) = dlsym(nvml.library, "nvmlDeviceGetName");
   *(void**)(&nvml.utilization) = dlsym(nvml.library, "nvmlDeviceGetUtilizationRates");
   *(void**)(&nvml.memory) = dlsym(nvml.library, "nvmlDeviceGetMemoryInfo");
   *(void**)(&nvml.temperature) = dlsym(nvml.library, "nvmlDeviceGetTemperature");
   *(void**)(&nvml.fan) = dlsym(nvml.library, "nvmlDeviceGetFanSpeed");
   *(void**)(&nvml.rpm) = dlsym(nvml.library, "nvmlDeviceGetFanSpeedRPM");
   *(void**)(&nvml.power) = dlsym(nvml.library, "nvmlDeviceGetPowerUsage");
   *(void**)(&nvml.limit) = dlsym(nvml.library, "nvmlDeviceGetEnforcedPowerLimit");

   if (!nvml.init || !nvml.shutdown || !nvml.count || !nvml.handle || nvml.init() != 0) {
      xSnprintf(telemetry.message, sizeof(telemetry.message), "NVIDIA driver initialization failed");
      dlclose(nvml.library);
      nvml.library = NULL;
      return false;
   }
   nvml.initialized = true;
   return true;
#endif
}

const NvidiaTelemetry* NvidiaTelemetry_get(void) {
   double now = NvidiaTelemetry_now();
   if (now < nextPoll)
      return &telemetry;
   nextPoll = now + 1.0;

   if (!nvml.initialized && !NvidiaTelemetry_init()) {
      nextPoll = now + 5.0;
      return &telemetry;
   }

   unsigned int count = 0;
   if (nvml.count(&count) != 0) {
      for (unsigned int i = 0; i < telemetry.count; i++)
         telemetry.gpus[i].available = false;
      xSnprintf(telemetry.message, sizeof(telemetry.message), "NVIDIA device query failed");
      return &telemetry;
   }
   telemetry.count = count < NVIDIA_MAX_GPUS ? count : NVIDIA_MAX_GPUS;
   xSnprintf(telemetry.message, sizeof(telemetry.message), "%s", count ? "" : "No NVIDIA GPUs detected");

   for (unsigned int i = 0; i < telemetry.count; i++) {
      NvidiaGPU* gpu = &telemetry.gpus[i];
      *gpu = (NvidiaGPU) {
         .utilization = NAN, .memoryUsedMiB = NAN, .memoryTotalMiB = NAN,
         .temperature = NAN, .fanPercent = NAN, .fanRPM = NAN,
         .power = NAN, .powerLimit = NAN,
      };
      xSnprintf(gpu->name, sizeof(gpu->name), "NVIDIA GPU %u", i);
      NvidiaDevice device;
      if (nvml.handle(i, &device) != 0)
         continue;
      gpu->available = true;
      if (nvml.name)
         nvml.name(device, gpu->name, sizeof(gpu->name));
      gpu->name[sizeof(gpu->name) - 1] = '\0';

      NvidiaUtilization utilization;
      if (nvml.utilization && nvml.utilization(device, &utilization) == 0)
         gpu->utilization = utilization.gpu;
      NvidiaMemory memory;
      if (nvml.memory && nvml.memory(device, &memory) == 0 && memory.total > 0) {
         gpu->memoryUsedMiB = memory.used / 1048576.0;
         gpu->memoryTotalMiB = memory.total / 1048576.0;
      }
      unsigned int value;
      if (nvml.temperature && nvml.temperature(device, 0, &value) == 0)
         gpu->temperature = value;
      if (nvml.fan && nvml.fan(device, &value) == 0)
         gpu->fanPercent = value;
      NvidiaFanSpeed fan = { .version = sizeof(fan) | (1U << 24), .fan = 0 };
      if (nvml.rpm && nvml.rpm(device, &fan) == 0)
         gpu->fanRPM = fan.speed;
      if (nvml.power && nvml.power(device, &value) == 0)
         gpu->power = value / 1000.0;
      if (nvml.limit && nvml.limit(device, &value) == 0 && value > 0)
         gpu->powerLimit = value / 1000.0;
   }
   return &telemetry;
}

void NvidiaTelemetry_done(void) {
   if (nvml.initialized)
      nvml.shutdown();
#ifndef BUILD_STATIC
   if (nvml.library)
      dlclose(nvml.library);
#endif
   nvml.library = NULL;
   nvml.initialized = false;
   telemetry.count = 0;
   nextPoll = 0;
}
