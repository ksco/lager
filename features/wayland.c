#define _GNU_SOURCE
#include "wayland.h"

#include "common.h"

#include "../guest/services.h"
#include "../misc/log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void feature_wayland_host_resolve(struct host_ctx *ctx)
{
    if (ctx->display != DISPLAY_WAYLAND)
        return;
    feature_require_executable("/usr/bin/weston", "weston", "wayland support", "weston");
    feature_require_executable("/usr/bin/seatd", "seatd", "wayland support", "seatd");
    feature_require_executable("/usr/lib/systemd/systemd-udevd", "systemd-udevd", "wayland support", "systemd");
    feature_require_executable("/usr/bin/udevadm", "udevadm", "wayland support", "systemd");
}

void feature_wayland_host_add_env(struct host_ctx *ctx)
{
    env_unset(ctx->env, "DISPLAY");
    env_unset(ctx->env, "XAUTHORITY");
    env_set(ctx->env, "WAYLAND_DISPLAY=/run/lager/wayland-1");
    env_set(ctx->env, "XDG_SESSION_TYPE=wayland");
    env_set(ctx->env, "GDK_BACKEND=wayland");
    env_set(ctx->env, "QT_QPA_PLATFORM=wayland");
    env_set(ctx->env, "SDL_VIDEODRIVER=wayland");
    env_set(ctx->env, "CLUTTER_BACKEND=wayland");
    env_set(ctx->env, "ELECTRON_OZONE_PLATFORM_HINT=wayland");
}

void feature_wayland_host_add_qemu_options(struct host_ctx *ctx)
{
    (void)ctx;
    vec_push_copy(ctx->qemu, "-device");
    vec_push_copy(ctx->qemu, "virtio-keyboard-pci");
    vec_push_copy(ctx->qemu, "-device");
    vec_push_copy(ctx->qemu, "virtio-tablet-pci");
}

static void setup_guest_udev(void)
{
    char *daemon[] = {"/usr/lib/systemd/systemd-udevd", "--daemon", NULL};
    char *trigger[] = {"/usr/bin/udevadm", "trigger", "--action=add", NULL};
    char *settle[] = {"/usr/bin/udevadm", "settle", NULL};

    mkdir_ok("/run/udev", 0755);
    if (feature_spawn_wait(daemon, false) < 0 || feature_spawn_wait(trigger, false) < 0 ||
        feature_spawn_wait(settle, false) < 0)
        die("cannot initialize guest input devices");
}

static void setup_guest_wayland(int log_fd)
{
    const char *socket_path = "/run/lager/wayland-1";
    int tries;

    setup_guest_udev();
    mkdir_ok("/tmp/.X11-unix", 01777);
    mkdir_ok("/run/lager", 0755);
    unlink(socket_path);
    if (guest_service_fork(GUEST_SERVICE_SEATD) == 0) {
        silence_output_fd(log_fd);
        execl("/usr/bin/seatd", "seatd", (char *)NULL);
        fprintf(stderr, "lager: exec seatd: %s\n", strerror(errno));
        _exit(127);
    }
    guest_service_wait_path(GUEST_SERVICE_SEATD, "/run/seatd.sock");
    if (guest_service_fork(GUEST_SERVICE_WESTON) == 0) {
        setenv("HOME", "/root", 1);
        setenv("XDG_RUNTIME_DIR", "/run/lager", 1);
        silence_output_fd(log_fd);
        execl("/usr/bin/weston", "weston", (char *)NULL);
        fprintf(stderr, "lager: exec weston: %s\n", strerror(errno));
        _exit(127);
    }
    for (tries = 0; tries < 100; tries++) {
        if (path_exists(socket_path)) {
            if (chmod(socket_path, 0666) < 0)
                warnx("cannot chmod %s: %s", socket_path, strerror(errno));
            return;
        }
        if (guest_service_exited(GUEST_SERVICE_WESTON))
            die("weston exited before creating %s; see logs", socket_path);
        usleep(50000);
    }
    die("weston did not create %s; see logs", socket_path);
}

void feature_wayland_guest_setup(struct guest_ctx *ctx)
{
    setup_guest_wayland(ctx->log_fd);
}

void feature_wayland_guest_stop(struct guest_ctx *ctx)
{
    (void)ctx;
    guest_service_stop(GUEST_SERVICE_WESTON);
    guest_service_stop(GUEST_SERVICE_SEATD);
}
