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
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

void feature_wayland_host_resolve(struct host_ctx *ctx)
{
    (void)ctx;
    feature_require_executable("/usr/bin/weston", "weston", "wayland support", "weston");
    feature_require_executable("/usr/bin/seatd", "seatd", "wayland support", "seatd");
    feature_require_executable("/usr/bin/Xwayland", "Xwayland", "wayland support", "xwayland");
    feature_require_executable("/usr/lib/systemd/systemd-udevd", "systemd-udevd", "wayland support", "systemd");
    feature_require_executable("/usr/bin/udevadm", "udevadm", "wayland support", "systemd");
}

void feature_wayland_host_add_env(struct host_ctx *ctx)
{
    env_set(ctx->env, "DISPLAY=:0");
    env_set(ctx->env, "WAYLAND_DISPLAY=/run/lager/wayland-1");
    env_unset(ctx->env, "XAUTHORITY");
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
    vec_push_copy(ctx->qemu, "virtio-mouse-pci");
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
    const char *x11_socket = "/tmp/.X11-unix/X0";
    const char *wrapper_path = "/run/lager/Xwayland";
    const char *real_path = "/run/lager/Xwayland.real";
    const char *weston_ini = "/run/lager/weston.ini";
    int fd, tries;

    setup_guest_udev();
    mkdir_ok("/tmp/.X11-unix", 01777);
    mkdir_ok("/run/lager", 0755);
    unlink(socket_path);
    unlink(x11_socket);
    unlink(wrapper_path);
    unlink(real_path);
    unlink(weston_ini);

    /* Copy Xwayland binary so we can wrap it */
    {
        char *cp[] = {"cp", "-a", "/usr/bin/Xwayland", (char *)real_path, NULL};
        if (feature_spawn_wait(cp, false) < 0)
            die("cannot copy Xwayland binary");
    }

    /* Create wrapper script that adds -ac (disable X11 auth) */
    fd = open(wrapper_path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0)
        die("cannot create Xwayland wrapper: %s", strerror(errno));
    const char wrapper[] = "#!/bin/sh\nexec -a \"$0\" /run/lager/Xwayland.real -ac \"$@\"\n";
    if (write(fd, wrapper, sizeof(wrapper) - 1) < 0)
        die("cannot write Xwayland wrapper: %s", strerror(errno));
    close(fd);

    /* Bind-mount wrapper over the real binary so weston uses it */
    if (mount(wrapper_path, "/usr/bin/Xwayland", NULL, MS_BIND, NULL) < 0)
        die("cannot bind-mount Xwayland wrapper: %s", strerror(errno));

    /* Write weston.ini to disable idle/sleep and the top panel */
    fd = open(weston_ini, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        die("cannot create weston.ini: %s", strerror(errno));
    const char weston_config[] =
        "[core]\n"
        "idle-time=0\n"
        "xwayland=true\n"
        "\n"
        "[shell]\n"
        "panel-position=none\n"
        "clock-format=none\n";
    if (write(fd, weston_config, sizeof(weston_config) - 1) < 0)
        die("cannot write weston.ini: %s", strerror(errno));
    close(fd);

    if (guest_service_fork(GUEST_SERVICE_SEATD) == 0) {
        silence_output_fd(log_fd);
        execl("/usr/bin/seatd", "seatd", (char *)NULL);
        fprintf(stderr, "lager: exec seatd: %s\n", strerror(errno));
        _exit(127);
    }
    guest_service_wait_path(GUEST_SERVICE_SEATD, "/run/seatd.sock");

    if (guest_service_fork(GUEST_SERVICE_WESTON) == 0) {
        setenv("HOME", "/run/lager", 1);
        setenv("XDG_RUNTIME_DIR", "/run/lager", 1);
        setenv("XDG_CONFIG_HOME", "/run/lager", 1);
        silence_output_fd(log_fd);
        execl("/usr/bin/weston", "weston", (char *)NULL);
        fprintf(stderr, "lager: exec weston: %s\n", strerror(errno));
        _exit(127);
    }

    for (tries = 0; tries < 100; tries++) {
        if (path_exists(socket_path)) {
            if (chmod(socket_path, 0666) < 0)
                warnx("cannot chmod %s: %s", socket_path, strerror(errno));
            break;
        }
        if (guest_service_exited(GUEST_SERVICE_WESTON))
            die("weston exited before creating %s; see logs", socket_path);
        usleep(50000);
    }
    if (!path_exists(socket_path))
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
