#define _GNU_SOURCE
#include "x11.h"

#include "common.h"

#include "../guest_services.h"
#include "../log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void feature_x11_host_resolve(struct host_ctx *ctx)
{
    if (!ctx->x11)
        return;
    feature_require_executable("/usr/libexec/Xorg", "Xorg", "x11 support", "xorg-server");
    feature_require_executable("/usr/lib/systemd/systemd-udevd", "systemd-udevd", "x11 support", "systemd");
    feature_require_executable("/usr/bin/udevadm", "udevadm", "x11 support", "systemd");
}

void feature_x11_host_add_env(struct host_ctx *ctx)
{
    env_unset(ctx->env, "WAYLAND_DISPLAY");
    env_unset(ctx->env, "WAYLAND_SOCKET");
    env_set(ctx->env, "DISPLAY=:1");
    env_set(ctx->env, "XAUTHORITY=");
    env_set(ctx->env, "XDG_SESSION_TYPE=x11");
    env_set(ctx->env, "GDK_BACKEND=x11");
    env_set(ctx->env, "QT_QPA_PLATFORM=xcb");
    env_set(ctx->env, "SDL_VIDEODRIVER=x11");
    env_set(ctx->env, "CLUTTER_BACKEND=x11");
    env_set(ctx->env, "ELECTRON_OZONE_PLATFORM_HINT=x11");
}

void feature_x11_host_add_qemu_options(struct host_ctx *ctx)
{
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

static void write_xorg_config(uint32_t width, uint32_t height)
{
    int fd;
    char *config;

    fd = open("/run/lager/xorg.conf", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        die("create /run/lager/xorg.conf: %s", strerror(errno));
    config = xasprintf("Section \"Device\"\n"
                       "    Identifier \"Device0\"\n"
                       "EndSection\n"
                       "Section \"Monitor\"\n"
                       "    Identifier \"Monitor0\"\n"
                       "EndSection\n"
                       "Section \"Screen\"\n"
                       "    Identifier \"Screen0\"\n"
                       "    Device \"Device0\"\n"
                       "    Monitor \"Monitor0\"\n"
                       "    DefaultDepth 24\n"
                       "    SubSection \"Display\"\n"
                       "        Depth 24\n"
                       "        Modes \"%dx%d\"\n"
                       "    EndSubSection\n"
                       "EndSection\n",
                       (int)width, (int)height);
    write_all(fd, config, strlen(config));
    close(fd);
    free(config);
}

static void setup_guest_x11(uint32_t width, uint32_t height, int log_fd)
{
    const char *socket_path = "/tmp/.X11-unix/X1";
    const char *config_path = NULL;
    int tries;

    mkdir_ok("/tmp/.X11-unix", 01777);
    mkdir_ok("/run/lager", 0755);
    mkdir_ok("/run/lager/cache", 0700);
    if (width > 0 && height > 0) {
        write_xorg_config(width, height);
        config_path = "/run/lager/xorg.conf";
    }
    unlink(socket_path);
    if (guest_service_fork(GUEST_SERVICE_XORG) == 0) {
        setenv("HOME", "/root", 1);
        setenv("XDG_CACHE_HOME", "/run/lager/cache", 1);
        silence_output_fd(log_fd);
        if (config_path)
            execl("/usr/libexec/Xorg", "Xorg", ":1", "-ac", "-noreset", "-nolisten", "tcp", "-logfile",
                  "/run/lager/Xorg.log", "-config", config_path, (char *)NULL);
        else
            execl("/usr/libexec/Xorg", "Xorg", ":1", "-ac", "-noreset", "-nolisten", "tcp", "-logfile",
                  "/run/lager/Xorg.log", (char *)NULL);
        fprintf(stderr, "lager: exec Xorg: %s\n", strerror(errno));
        _exit(127);
    }
    for (tries = 0; tries < 100; tries++) {
        if (path_exists(socket_path))
            return;
        if (guest_service_exited(GUEST_SERVICE_XORG))
            die("Xorg exited before creating %s; see /run/lager/Xorg.log", socket_path);
        usleep(50000);
    }
    die("Xorg did not create %s; see /run/lager/Xorg.log", socket_path);
}

void feature_x11_guest_setup(struct guest_ctx *ctx)
{
    setup_guest_udev();
    setup_guest_x11(ctx->cfg->header.resolution_width, ctx->cfg->header.resolution_height, ctx->log_fd);
}

void feature_x11_guest_stop(struct guest_ctx *ctx)
{
    (void)ctx;
    guest_service_stop(GUEST_SERVICE_XORG);
}
