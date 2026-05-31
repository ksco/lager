#define _GNU_SOURCE
#include "power.h"

#include "common.h"

#include "../guest_services.h"
#include "../log.h"

#include <errno.h>
#include <grp.h>
#include <string.h>
#include <unistd.h>

static void setup_guest_polkit(int log_fd)
{
    uid_t uid;
    gid_t gid;

    if (!executable("/usr/lib/polkit-1/polkitd"))
        return;
    if (!feature_guest_user_ids("polkitd", &uid, &gid)) {
        warnx("cannot find polkitd user");
        return;
    }
    if (guest_service_fork(GUEST_SERVICE_POLKIT) == 0) {
        if (setgroups(0, NULL) < 0 || setgid(gid) < 0 || setuid(uid) < 0)
            die("drop polkitd privileges: %s", strerror(errno));
        silence_output_fd(log_fd);
        execl("/usr/lib/polkit-1/polkitd", "polkitd", "--no-debug",
              (char *)NULL);
        die("exec polkitd: %s", strerror(errno));
    }
    if (!feature_wait_for_guest_bus_name("--system",
                                         "org.freedesktop.PolicyKit1",
                                         GUEST_SERVICE_POLKIT, "polkitd", 0, 0))
        guest_service_forget(GUEST_SERVICE_POLKIT);
}

static void setup_guest_upower(int log_fd)
{
    if (!executable("/usr/libexec/upowerd"))
        return;
    if (guest_service_fork(GUEST_SERVICE_UPOWER) == 0) {
        silence_output_fd(log_fd);
        execl("/usr/libexec/upowerd", "upowerd", (char *)NULL);
        die("exec upowerd: %s", strerror(errno));
    }
    if (!feature_wait_for_guest_bus_name("--system", "org.freedesktop.UPower",
                                         GUEST_SERVICE_UPOWER, "upowerd", 0, 0))
        guest_service_forget(GUEST_SERVICE_UPOWER);
}

void feature_power_guest_setup(struct guest_ctx *ctx)
{
    setup_guest_polkit(ctx->log_fd);
    setup_guest_upower(ctx->log_fd);
}

void feature_power_guest_stop(struct guest_ctx *ctx)
{
    (void)ctx;
    guest_service_stop(GUEST_SERVICE_UPOWER);
    guest_service_stop(GUEST_SERVICE_POLKIT);
}
