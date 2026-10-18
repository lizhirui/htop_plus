#ifndef HEADER_CableConfig
#define HEADER_CableConfig
/* htop - released under the GNU GPLv2+, see COPYING. */

#include <stdbool.h>
#include <stddef.h>
#include <limits.h>

typedef struct CableConfig_ {
   char serial[256];
   char vid[5];
   char pid[5];
   char port[PATH_MAX];
   double timeout;
} CableConfig;

bool CableConfig_load(const char* path, CableConfig* config, char* error, size_t size);
bool CableConfig_resolve(const CableConfig* config, const char* sysRoot, const char* devRoot,
                         char* port, size_t portSize, char* error, size_t errorSize);

#endif
