#ifndef LAGER_GUEST_SERVICES_H
#define LAGER_GUEST_SERVICES_H

#include <stdbool.h>
#include <sys/types.h>

enum guest_service_id {
    GUEST_SERVICE_XORG,
    GUEST_SERVICE_OPENBOX,
    GUEST_SERVICE_DUNST,
    GUEST_SERVICE_SYSTEM_BUS,
    GUEST_SERVICE_SESSION_BUS,
    GUEST_SERVICE_AUDIO,
    GUEST_SERVICE_WESTON,
    GUEST_SERVICE_SEATD,
    GUEST_SERVICE_XWAYLAND,
};

pid_t guest_service_fork(enum guest_service_id id);
pid_t guest_service_pid(enum guest_service_id id);
bool guest_service_exited(enum guest_service_id id);
void guest_service_forget(enum guest_service_id id);
void guest_service_wait_path(enum guest_service_id id, const char *path);
void guest_service_stop(enum guest_service_id id);

#endif
