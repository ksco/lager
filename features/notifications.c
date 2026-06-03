#define _GNU_SOURCE
#include "notifications.h"

#include "common.h"

#include "../guest/services.h"
#include "../misc/log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

void feature_notifications_host_resolve(struct host_ctx *ctx)
{
    if (ctx->display == DISPLAY_NONE)
        return;
    feature_require_executable("/usr/bin/dunst", "dunst", "notifications", "dunst");
}

static void setup_guest_notifications(const struct guest_config *cfg, int log_fd)
{
    char *session_bus = xasprintf("--bus=unix:path=/run/user/%u/bus", cfg->header.uid);

    if (guest_service_fork(GUEST_SERVICE_DUNST) == 0) {
        if (setgid((gid_t)cfg->header.gid) < 0 || setuid((uid_t)cfg->header.uid) < 0)
            die("drop dunst privileges: %s", strerror(errno));
        environ = cfg->env;
        silence_output_fd(log_fd);
        execl("/usr/bin/dunst", "dunst", (char *)NULL);
        die("exec dunst: %s", strerror(errno));
    }
    if (!feature_wait_for_guest_bus_name(session_bus, "org.freedesktop.Notifications", GUEST_SERVICE_DUNST, "dunst",
                                         (uid_t)cfg->header.uid, (gid_t)cfg->header.gid))
        die("cannot start dunst");
    free(session_bus);
}

void feature_notifications_guest_setup(struct guest_ctx *ctx)
{
    setup_guest_notifications(ctx->cfg, ctx->log_fd);
}

void feature_notifications_guest_stop(struct guest_ctx *ctx)
{
    (void)ctx;
    guest_service_stop(GUEST_SERVICE_DUNST);
}
