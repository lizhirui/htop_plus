#ifndef HEADER_PowerCable
#define HEADER_PowerCable
/*
htop - linux/PowerCable.h
Released under the GNU GPLv2+, see the COPYING file.
*/

#include <stdbool.h>


typedef struct PowerCableReading_ {
   double voltage[6];
   double current[6];
   double power;
   double connectorTemperature;
   double controllerTemperature;
   double updated;
   bool valid;
   char message[160];
} PowerCableReading;

/* These functions never wait for serial I/O. Call get on each screen refresh. */
const PowerCableReading* PowerCable_get(void);
void PowerCable_done(void);

#endif
