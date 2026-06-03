#define _GNU_SOURCE
#include "services.h"

#include "../misc/log.h"
#include "../misc/utils.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

struct guest_service {
    const char *name;
    pid_t pid;
};

static struct guest_service services[] = {
    [GUEST_SERVICE_XORG] = {"Xorg", -1},
    [GUEST_SERVICE_OPENBOX] = {"openbox", -1},
    [GUEST_SERVICE_DUNST] = {"dunst", -1},
    [GUEST_SERVICE_SYSTEM_BUS] = {"system dbus-daemon", -1},
    [GUEST_SERVICE_SESSION_BUS] = {"session dbus-daemon", -1},
    [GUEST_SERVICE_AUDIO] = {"pulseaudio", -1},
    [GUEST_SERVICE_WESTON] = {"weston", -1},
    [GUEST_SERVICE_SEATD] = {"seatd", -1},
    [GUEST_SERVICE_XWAYLAND] = {"Xwayland", -1},
};

static struct guest_service *service(enum guest_service_id id)
{
    if ((size_t)id >= sizeof(services) / sizeof(services[0]))
        die("invalid guest service");
    return &services[id];
}

pid_t guest_service_fork(enum guest_service_id id)
{
    struct guest_service *entry = service(id);

    entry->pid = fork();
    if (entry->pid < 0)
        die("fork %s: %s", entry->name, strerror(errno));
    return entry->pid;
}

pid_t guest_service_pid(enum guest_service_id id)
{
    return service(id)->pid;
}

bool guest_service_exited(enum guest_service_id id)
{
    struct guest_service *entry = service(id);
    int status;

    if (entry->pid <= 0)
        return true;
    if (waitpid(entry->pid, &status, WNOHANG) != entry->pid)
        return false;
    entry->pid = -1;
    return true;
}

void guest_service_forget(enum guest_service_id id)
{
    service(id)->pid = -1;
}

void guest_service_wait_path(enum guest_service_id id, const char *path)
{
    struct guest_service *entry = service(id);
    int tries;

    for (tries = 0; tries < 100; tries++) {
        if (path_exists(path))
            return;
        if (guest_service_exited(id))
            die("%s exited before creating %s", entry->name, path);
        usleep(50000);
    }
    die("%s did not create %s", entry->name, path);
}

void guest_service_stop(enum guest_service_id id)
{
    struct guest_service *entry = service(id);

    if (entry->pid <= 0)
        return;
    kill(entry->pid, SIGTERM);
    while (waitpid(entry->pid, NULL, 0) < 0 && errno == EINTR)
        ;
    entry->pid = -1;
}
