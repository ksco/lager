#define _GNU_SOURCE
#include "dbus.h"

#include "common.h"

#include "../guest_services.h"
#include "../log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern char **environ;

void feature_dbus_host_resolve(struct host_ctx *ctx)
{
    (void)ctx;
    feature_require_executable("/usr/bin/dbus-daemon", "dbus-daemon", "dbus support", "dbus");
    feature_require_executable("/usr/bin/dbus-send", "dbus-send", "dbus support", "dbus");
}

void feature_dbus_host_add_env(struct host_ctx *ctx)
{
    char *assignment = xasprintf("DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%lu/bus", (unsigned long)getuid());

    env_set(ctx->env, assignment);
    free(assignment);
}

static void setup_guest_dbus(const struct guest_config *cfg, int log_fd)
{
    const char *system_socket = "/run/dbus/system_bus_socket";
    char *runtime_dir = xasprintf("/run/user/%u", cfg->header.uid);
    char *session_socket = xasprintf("%s/bus", runtime_dir);
    char *session_address = xasprintf("--address=unix:path=%s", session_socket);

    mkdir_ok("/run/dbus", 0755);
    mkdir_ok("/run/user", 0755);
    mkdir_ok(runtime_dir, 0700);
    if (chmod(runtime_dir, 0700) < 0 || chown(runtime_dir, (uid_t)cfg->header.uid, (gid_t)cfg->header.gid) < 0)
        die("prepare %s: %s", runtime_dir, strerror(errno));
    unlink(system_socket);
    if (guest_service_fork(GUEST_SERVICE_SYSTEM_BUS) == 0) {
        silence_output_fd(log_fd);
        execl("/usr/bin/dbus-daemon", "dbus-daemon", "--system", "--nofork", "--nopidfile", (char *)NULL);
        die("exec system dbus-daemon: %s", strerror(errno));
    }
    guest_service_wait_path(GUEST_SERVICE_SYSTEM_BUS, system_socket);

    unlink(session_socket);
    if (guest_service_fork(GUEST_SERVICE_SESSION_BUS) == 0) {
        if (setgid((gid_t)cfg->header.gid) < 0 || setuid((uid_t)cfg->header.uid) < 0)
            die("drop session dbus-daemon privileges: %s", strerror(errno));
        environ = cfg->env;
        silence_output_fd(log_fd);
        execl("/usr/bin/dbus-daemon", "dbus-daemon", "--session", "--nofork", "--nopidfile", session_address,
              (char *)NULL);
        die("exec session dbus-daemon: %s", strerror(errno));
    }
    guest_service_wait_path(GUEST_SERVICE_SESSION_BUS, session_socket);
    free(session_address);
    free(session_socket);
    free(runtime_dir);
}

void feature_dbus_guest_setup(struct guest_ctx *ctx)
{
    setup_guest_dbus(ctx->cfg, ctx->log_fd);
}

void feature_dbus_guest_stop(struct guest_ctx *ctx)
{
    (void)ctx;
    guest_service_stop(GUEST_SERVICE_SESSION_BUS);
    guest_service_stop(GUEST_SERVICE_SYSTEM_BUS);
}
