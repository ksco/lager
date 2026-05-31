#ifndef LAGER_FEATURES_COMMON_H
#define LAGER_FEATURES_COMMON_H

#include "../guest_services.h"

#include <stdbool.h>
#include <sys/types.h>

void feature_require_executable(const char *path, const char *program,
                                const char *feature, const char *package);
int feature_spawn_wait(char *const argv[], bool warn_failure);
int feature_spawn_wait_silent(char *const argv[], uid_t uid, gid_t gid);
bool feature_wait_for_guest_bus_name(const char *bus, const char *name,
                                     enum guest_service_id service_id,
                                     const char *process_name, uid_t uid,
                                     gid_t gid);
bool feature_guest_user_ids(const char *wanted, uid_t *uid, gid_t *gid);

#endif
