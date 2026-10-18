#ifndef HEADER_DockerUsers
#define HEADER_DockerUsers
/* htop - released under the GNU GPLv2+, see COPYING. */

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct DockerUsers_ DockerUsers;

DockerUsers* DockerUsers_new(const char* socketPath);
void DockerUsers_delete(DockerUsers* this);
bool DockerUsers_format(DockerUsers* this, const char* cgroup, uid_t hostUid,
                        const char* uidMapPath, char* out, size_t size);

#endif
