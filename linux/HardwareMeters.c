/*
htop - linux/HardwareMeters.c
Released under the GNU GPLv2+, see the COPYING file.
*/

#include "config.h" // IWYU pragma: keep

#include "linux/HardwareMeters.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "CRT.h"
#include "Macros.h"
#include "Meter.h"
#include "Object.h"
#include "ProvideCurses.h"
#include "XUtils.h"
#include "linux/NvidiaTelemetry.h"
#include "linux/PowerCable.h"


static const int normalColor[] = { METER_VALUE_OK };
static const int memoryColor[] = { MEMORY_1 };
static const int warningColor[] = { METER_VALUE_WARN };
static const int errorColor[] = { METER_VALUE_ERROR };

/* Children use the same bar/text renderers as the CPU and memory meters. */
static void HardwareBar_updateValues(Meter* this ATTR_UNUSED) {
}

static const MeterClass HardwareBar_class = {
   .super = { .extends = Class(Meter), .delete = Meter_delete },
   .updateValues = HardwareBar_updateValues,
   .defaultMode = BAR_METERMODE,
   .supportedModes = (1 << BAR_METERMODE) | (1 << TEXT_METERMODE),
   .maxItems = 1,
   .isPercentChart = true,
   .total = 100.0,
   .attributes = normalColor,
   .name = "HardwareBar",
   .uiName = "Hardware bar",
   .caption = "",
};

static Meter* HardwareBar_new(const Meter* parent, const char* caption) {
   Meter* bar = Meter_new(parent->host, 0, &HardwareBar_class);
   Meter_setCaption(bar, caption);
   return bar;
}

static void HardwareBar_value(Meter* bar, double value, double total) {
   bar->values[0] = value;
   bar->total = isfinite(total) && total > 0 ? total : 100.0;
   bar->curAttributes = normalColor;
   if (!isfinite(value))
      xSnprintf(bar->txtBuffer, sizeof(bar->txtBuffer), "N/A");
}

static void HardwareBar_temperature(Meter* bar, double value) {
   HardwareBar_value(bar, value, 100.0);
   if (isfinite(value)) {
      xSnprintf(bar->txtBuffer, sizeof(bar->txtBuffer), "%.1fC", value);
      bar->curAttributes = value >= 85 ? errorColor : value >= 65 ? warningColor : normalColor;
   }
}

static void HardwareMeter_text(int x, int y, int w, const char* text, bool error) {
   if (w <= 0 || y < 0 || y >= LINES)
      return;
   attrset(CRT_colors[error ? METER_VALUE_WARN : METER_TEXT]);
   mvaddnstr(y, x, text, w);
   attrset(CRT_colors[RESET_COLOR]);
}

static void HardwareBar_draw(Meter* bar, int x, int y, int w) {
   if (w > 0 && y >= 0 && y < LINES)
      bar->draw(bar, x, y, w);
}

enum { GPU_CORE, GPU_MEMORY, GPU_TEMPERATURE, GPU_FAN, GPU_POWER, GPU_BARS };

typedef struct NvidiaMeters_ {
   Meter* bars[NVIDIA_MAX_GPUS][GPU_BARS];
   unsigned int count;
   NvidiaTelemetry telemetry;
} NvidiaMeters;

static void NvidiaGPUsMeter_init(Meter* this) {
   if (!this->meterData)
      this->meterData = xCalloc(1, sizeof(NvidiaMeters));
}

static void NvidiaGPUsMeter_updateValues(Meter* this) {
   const NvidiaTelemetry* telemetry = NvidiaTelemetry_get();
   NvidiaMeters* data = this->meterData;
   data->telemetry = *telemetry;
   data->count = telemetry->count;
   this->h = data->count ? (int)data->count * 6 : 2;
   for (unsigned int i = 0; i < data->count; i++) {
      static const char* const captions[] = { "GPU", "VRM", "Tmp", "Fan", "Pwr" };
      const NvidiaGPU* gpu = &telemetry->gpus[i];
      Meter** bars = data->bars[i];
      for (size_t j = 0; j < GPU_BARS; j++) {
         if (!bars[j])
            bars[j] = HardwareBar_new(this, captions[j]);
      }
      HardwareBar_value(bars[GPU_CORE], gpu->available ? gpu->utilization : NAN, 100);
      if (gpu->available && isfinite(gpu->utilization))
         xSnprintf(bars[GPU_CORE]->txtBuffer, METER_TXTBUFFER_LEN, "%.1f%%", gpu->utilization);
      HardwareBar_value(bars[GPU_MEMORY], gpu->available ? gpu->memoryUsedMiB : NAN, gpu->memoryTotalMiB);
      bars[GPU_MEMORY]->curAttributes = memoryColor;
      if (gpu->available && isfinite(gpu->memoryUsedMiB))
         xSnprintf(bars[GPU_MEMORY]->txtBuffer, METER_TXTBUFFER_LEN, "%.0f/%.0fMiB", gpu->memoryUsedMiB, gpu->memoryTotalMiB);
      HardwareBar_temperature(bars[GPU_TEMPERATURE], gpu->available ? gpu->temperature : NAN);
      HardwareBar_value(bars[GPU_FAN], gpu->available ? gpu->fanPercent : NAN, 100);
      if (gpu->available && isfinite(gpu->fanPercent)) {
         xSnprintf(bars[GPU_FAN]->txtBuffer, METER_TXTBUFFER_LEN, "%.0f%%", gpu->fanPercent);
         if (isfinite(gpu->fanRPM))
            xSnprintf(bars[GPU_FAN]->txtBuffer, METER_TXTBUFFER_LEN, "%.0f%% %.0frpm", gpu->fanPercent, gpu->fanRPM);
      } else if (gpu->available && isfinite(gpu->fanRPM)) {
         xSnprintf(bars[GPU_FAN]->txtBuffer, METER_TXTBUFFER_LEN, "%.0frpm", gpu->fanRPM);
      }
      HardwareBar_value(bars[GPU_POWER], gpu->available ? gpu->power : NAN, gpu->powerLimit);
      if (gpu->available && isfinite(gpu->power)) {
         if (isfinite(gpu->powerLimit)) {
            xSnprintf(bars[GPU_POWER]->txtBuffer, METER_TXTBUFFER_LEN, "%.1f/%.0fW", gpu->power, gpu->powerLimit);
         } else {
            /* No invented limit: no filled bar when the driver cannot report it. */
            bars[GPU_POWER]->values[0] = NAN;
            xSnprintf(bars[GPU_POWER]->txtBuffer, METER_TXTBUFFER_LEN, "%.1fW (limit N/A)", gpu->power);
         }
      }
   }
}

static void NvidiaGPUsMeter_draw(Meter* this, int x, int y, int w) {
   const NvidiaMeters* data = this->meterData;
   const NvidiaTelemetry* telemetry = &data->telemetry;
   if (!data->count) {
      HardwareMeter_text(x, y, w, "NVIDIA GPUs", false);
      HardwareMeter_text(x, y + 1, w, telemetry->message, true);
      return;
   }
   for (unsigned int i = 0; i < data->count; i++) {
      const NvidiaGPU* gpu = &telemetry->gpus[i];
      char name[160];
      const char* model = gpu->name;
      if ((int)strlen(model) + 5 > w && String_startsWith(model, "NVIDIA "))
         model += 7;
      if ((int)strlen(model) + 5 > w && String_startsWith(model, "GeForce "))
         model += 8;
      xSnprintf(name, sizeof(name), "GPU%u %s%s", i, model, gpu->available ? "" : " [offline]");
      HardwareMeter_text(x, y, w, name, !gpu->available);
      Meter* const* bars = data->bars[i];
      HardwareBar_draw(bars[GPU_CORE], x, y + 1, w);
      HardwareBar_draw(bars[GPU_MEMORY], x, y + 2, w);
      HardwareBar_draw(bars[GPU_TEMPERATURE], x, y + 3, w);
      HardwareBar_draw(bars[GPU_FAN], x, y + 4, w);
      HardwareBar_draw(bars[GPU_POWER], x, y + 5, w);
      y += 6;
   }
}

static void NvidiaGPUsMeter_done(Meter* this) {
   NvidiaMeters* data = this->meterData;
   for (size_t i = 0; i < NVIDIA_MAX_GPUS; i++)
      for (size_t j = 0; j < GPU_BARS; j++)
         Meter_delete((Object*)data->bars[i][j]);
   free(data);
}

static void HardwareMeter_updateMode(Meter* this, MeterModeId mode) {
   this->mode = mode;
}

typedef struct PowerCableMeters_ {
   Meter* channels[6];
   Meter* power;
   Meter* connector;
   Meter* controller;
   PowerCableReading reading;
} PowerCableMeters;

static size_t activeCableMeters;

static void PowerCableMeter_init(Meter* this) {
   if (this->meterData)
      return;
   PowerCableMeters* data = xCalloc(1, sizeof(PowerCableMeters));
   this->meterData = data;
   activeCableMeters++;
   for (size_t i = 0; i < 6; i++) {
      char caption[4] = { (char)('A' + i), ' ', ' ', '\0' };
      data->channels[i] = HardwareBar_new(this, caption);
   }
   data->power = HardwareBar_new(this, "Pwr");
   data->connector = HardwareBar_new(this, "Con");
   data->controller = HardwareBar_new(this, "Ctl");
   this->h = 11;
}

static void PowerCableMeter_updateValues(Meter* this) {
   const PowerCableReading* reading = PowerCable_get();
   PowerCableMeters* data = this->meterData;
   data->reading = *reading;
   for (size_t i = 0; i < 6; i++) {
      Meter* bar = data->channels[i];
      HardwareBar_value(bar, reading->valid ? reading->current[i] : NAN, 10.0);
      if (reading->valid) {
         xSnprintf(bar->txtBuffer, METER_TXTBUFFER_LEN, "%.2fV %.2fA %.1fW", reading->voltage[i], reading->current[i], reading->voltage[i] * reading->current[i]);
      }
   }
   HardwareBar_value(data->power, reading->valid ? reading->power : NAN, 600);
   if (reading->valid)
      xSnprintf(data->power->txtBuffer, METER_TXTBUFFER_LEN, "%.2fW /600W", reading->power);
   HardwareBar_temperature(data->connector, reading->valid ? reading->connectorTemperature : NAN);
   HardwareBar_temperature(data->controller, reading->valid ? reading->controllerTemperature : NAN);
}

static void PowerCableMeter_draw(Meter* this, int x, int y, int w) {
   const PowerCableMeters* data = this->meterData;
   const PowerCableReading* reading = &data->reading;
   HardwareMeter_text(x, y, w, "5090 / 12V-2X6 Ti", false);
   HardwareMeter_text(x, y + 1, w, reading->valid ? "V / A / W (10A scale)" : reading->message, !reading->valid);
   for (size_t i = 0; i < 6; i++)
      HardwareBar_draw(data->channels[i], x, y + 2 + (int)i, w);
   HardwareBar_draw(data->power, x, y + 8, w);
   HardwareBar_draw(data->connector, x, y + 9, w);
   HardwareBar_draw(data->controller, x, y + 10, w);
}

static void PowerCableMeter_done(Meter* this) {
   PowerCableMeters* data = this->meterData;
   for (size_t i = 0; i < 6; i++)
      Meter_delete((Object*)data->channels[i]);
   Meter_delete((Object*)data->power);
   Meter_delete((Object*)data->connector);
   Meter_delete((Object*)data->controller);
   free(data);
   if (--activeCableMeters == 0)
      PowerCable_done();
}

const MeterClass NvidiaGPUsMeter_class = {
   .super = { .extends = Class(Meter), .delete = Meter_delete },
   .init = NvidiaGPUsMeter_init,
   .done = NvidiaGPUsMeter_done,
   .updateValues = NvidiaGPUsMeter_updateValues,
   .updateMode = HardwareMeter_updateMode,
   .draw = NvidiaGPUsMeter_draw,
   .defaultMode = BAR_METERMODE,
   .supportedModes = (1 << BAR_METERMODE),
   .isMultiColumn = true,
   .attributes = normalColor,
   .name = "NvidiaGPUs",
   .uiName = "NVIDIA GPU sensors",
   .description = "All NVIDIA GPUs: name, usage, VRAM, temperature, fan and power",
   .caption = "GPU",
};

const MeterClass PowerCableMeter_class = {
   .super = { .extends = Class(Meter), .delete = Meter_delete },
   .init = PowerCableMeter_init,
   .done = PowerCableMeter_done,
   .updateValues = PowerCableMeter_updateValues,
   .updateMode = HardwareMeter_updateMode,
   .draw = PowerCableMeter_draw,
   .defaultMode = BAR_METERMODE,
   .supportedModes = (1 << BAR_METERMODE),
   .isMultiColumn = true,
   .attributes = normalColor,
   .name = "PowerCable",
   .uiName = "5090 12V-2X6 Ti cable",
   .description = "External cable: six V/A/W channels, total power, connector/controller temperature",
   .caption = "12V",
};
