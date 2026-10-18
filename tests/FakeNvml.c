/* Deterministic public-ABI fixture; no GPU or NVIDIA driver is needed. */
#include <stdio.h>
#include <stdlib.h>

typedef void* Device;
typedef struct { unsigned int gpu, memory; } Utilization;
typedef struct { unsigned long long total, free, used; } Memory;
typedef struct { unsigned int version, fan, speed; } Fan;

int nvmlInit_v2(void) { return getenv("TEST_NVML_INIT_FAIL") ? 9 : 0; }
int nvmlShutdown(void) { return 0; }
int nvmlDeviceGetCount_v2(unsigned int* count) { *count = 2; return 0; }
int nvmlDeviceGetHandleByIndex_v2(unsigned int index, Device* device) { *device = (Device)(size_t)(index + 1); return 0; }
int nvmlDeviceGetName(Device device, char* name, unsigned int size) {
   snprintf(name, size, "NVIDIA GeForce RTX %s", (size_t)device == 1 ? "2080 Ti" : "5090");
   return 0;
}
int nvmlDeviceGetUtilizationRates(Device device, Utilization* utilization) {
   if ((size_t)device == 2) return 3;
   *utilization = (Utilization) { .gpu = 42, .memory = 12 }; return 0;
}
int nvmlDeviceGetMemoryInfo(Device device, Memory* memory) {
   (void)device;
   *memory = (Memory) { .total = 32ULL << 30, .free = 12ULL << 30, .used = 20ULL << 30 }; return 0;
}
int nvmlDeviceGetTemperature(Device device, unsigned int sensor, unsigned int* value) {
   (void)device; (void)sensor; *value = 54; return 0;
}
int nvmlDeviceGetFanSpeed(Device device, unsigned int* value) { (void)device; *value = 35; return 0; }
int nvmlDeviceGetFanSpeedRPM(Device device, Fan* fan) {
   (void)device;
   if (fan->version != (sizeof(*fan) | (1U << 24)) || fan->fan != 0) return 25;
   fan->speed = 1200; return 0;
}
int nvmlDeviceGetPowerUsage(Device device, unsigned int* value) { (void)device; *value = 123450; return 0; }
int nvmlDeviceGetEnforcedPowerLimit(Device device, unsigned int* value) { (void)device; *value = 600000; return 0; }
