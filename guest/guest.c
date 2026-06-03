/*
 * lager - standalone LoongArch microVM launcher
 *
 * The same static binary is used as the host launcher and as the guest init.
 * The host side creates a small initramfs containing itself, a run-specific
 * configuration file, and the modules needed to mount the host root through
 * virtiofs. The guest side mounts that root, loads the remaining system kernel
 * modules, configures Box64 and Zink, and runs the requested command.
 */

#define _GNU_SOURCE
#include "../features/registry.h"
#include "../features/common.h"
#include "config.h"
#include "exec.h"
#include "guest.h"
#include "../misc/config.h"
#include "../misc/log.h"
#include "../misc/utils.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/reboot.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MODULE_INIT_COMPRESSED_FILE 4

static void set_guest_clock(const struct config_header *header)
{
    struct timespec uptime;
    struct timespec realtime = {
        .tv_sec = (time_t)header->realtime_sec,
        .tv_nsec = (long)header->realtime_nsec,
    };

    if (clock_gettime(CLOCK_MONOTONIC, &uptime) < 0) {
        warnx("cannot read guest uptime: %s", strerror(errno));
        return;
    }
    realtime.tv_sec += uptime.tv_sec;
    realtime.tv_nsec += uptime.tv_nsec;
    if (realtime.tv_nsec >= 1000000000L) {
        realtime.tv_sec++;
        realtime.tv_nsec -= 1000000000L;
    }
    if (clock_settime(CLOCK_REALTIME, &realtime) < 0)
        warnx("cannot set guest clock: %s", strerror(errno));
}

static void mount_ok(const char *source, const char *target, const char *type, unsigned long flags, const char *data)
{
    mkdir_ok(target, 0755);
    if (mount(source, target, type, flags, data) < 0 && errno != EBUSY)
        die("mount %s on %s: %s", source ? source : type, target, strerror(errno));
}

static void symlink_ok(const char *target, const char *path)
{
    if (symlink(target, path) < 0 && errno != EEXIST)
        die("symlink %s: %s", path, strerror(errno));
}

static void setup_guest_dev_links(void)
{
    symlink_ok("/proc/self/fd", "/dev/fd");
    symlink_ok("/proc/self/fd/0", "/dev/stdin");
    symlink_ok("/proc/self/fd/1", "/dev/stdout");
    symlink_ok("/proc/self/fd/2", "/dev/stderr");
}

static void reopen_guest_console(void)
{
    int fd = open("/dev/hvc0", O_RDWR | O_NOCTTY);
    int stdio;

    if (fd < 0)
        fd = open("/dev/ttyS0", O_RDWR | O_NOCTTY);
    if (fd < 0)
        fd = open("/dev/console", O_RDWR | O_NOCTTY);
    if (fd < 0)
        return;
    setsid();
    ioctl(fd, TIOCSCTTY, 0);
    for (stdio = STDIN_FILENO; stdio <= STDERR_FILENO; stdio++)
        if (fd != stdio)
            dup2(fd, stdio);
    if (fd > STDERR_FILENO)
        close(fd);
}

static void load_early_module(const char *name)
{
    static const char *suffixes[] = {".ko.zst", ".ko.xz", ".ko.gz", ".ko"};
    size_t i;

    for (i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        char *path = xasprintf("/modules/%s%s", name, suffixes[i]);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        int flags = i + 1 < sizeof(suffixes) / sizeof(suffixes[0]) ? MODULE_INIT_COMPRESSED_FILE : 0;

        free(path);
        if (fd < 0)
            continue;
        if (syscall(SYS_finit_module, fd, "", flags) < 0 && errno != EEXIST)
            die("load %s: %s", name, strerror(errno));
        close(fd);
        return;
    }
    die("early module is missing: %s", name);
}

static void guest_modprobe(bool gpu, bool net, bool audio, bool display)
{
    struct strvec args = {0};

    vec_push_copy(&args, "/sbin/modprobe");
    vec_push_copy(&args, "-a");
    vec_push_copy(&args, "virtio_console");
    if (gpu)
        vec_push_copy(&args, "virtio_gpu");
    if (net)
        vec_push_copy(&args, "virtio_net");
    if (display)
        vec_push_copy(&args, "virtio_input");
    if (audio)
        vec_push_copy(&args, "virtio_snd");
    if (args.len > 2)
        feature_spawn_wait(args.items, true);
}

int guest_init(void)
{
    struct guest_config cfg;
    struct guest_ctx feature_ctx;
    int log_fd;
    int status;
    int tries;

    mkdir_ok("/proc", 0755);
    mount_ok("proc", "/proc", "proc", 0, NULL);
    load_early_module("virtio_pci_modern_dev");
    load_early_module("virtio_pci_legacy_dev");
    load_early_module("virtio_pci");
    cfg = read_guest_config();
    set_guest_clock(&cfg.header);
    load_early_module("fuse");
    load_early_module("virtiofs");
    if ((cfg.header.flags & CFG_GPU) && path_exists("/modules/virtio_gpu.ko")) {
        load_early_module("virtio_dma_buf");
        load_early_module("virtio_gpu");
    }
    mkdir_ok("/newroot", 0755);
    for (tries = 0; tries < 50; tries++) {
        if (mount(ROOT_TAG, "/newroot", "virtiofs", 0, NULL) == 0)
            break;
        usleep(100000);
    }
    if (tries == 50)
        die("mount virtiofs root: %s", strerror(errno));
    if (chdir("/newroot") < 0 || chroot(".") < 0 || chdir("/") < 0)
        die("switch to virtiofs root: %s", strerror(errno));
    log_fd = open_log(cfg.log_path);
    if (log_fd < 0)
        die("open %s: %s", cfg.log_path, strerror(errno));

    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
    mount_ok("devtmpfs", "/dev", "devtmpfs", 0, "mode=0755");
    mount_ok("proc", "/proc", "proc", 0, NULL);
    setup_guest_dev_links();
    mount_ok("sysfs", "/sys", "sysfs", 0, NULL);
    mkdir_ok("/dev/pts", 0755);
    mount_ok("devpts", "/dev/pts", "devpts", 0, "mode=0620,ptmxmode=0666");
    mount_ok("tmpfs", "/tmp", "tmpfs", 0, "mode=1777");
    mount_ok("tmpfs", "/run", "tmpfs", 0, "mode=0755");
    mkdir_ok("/dev/shm", 01777);
    mount_ok("tmpfs", "/dev/shm", "tmpfs", 0, "mode=1777");

    sethostname("lager", 5);
    guest_modprobe(cfg.header.flags & CFG_GPU, cfg.header.flags & CFG_NET, cfg.header.flags & CFG_AUDIO,
                   cfg.header.flags & (CFG_X11 | CFG_WAYLAND));
    reopen_guest_console();
    feature_ctx.cfg = &cfg;
    feature_ctx.log_fd = log_fd;
    features_guest_setup(&feature_ctx);
    close(log_fd);

    signal(SIGINT, guest_exec_signal);
    signal(SIGTERM, guest_exec_signal);
    signal(SIGHUP, guest_exec_signal);
    status = run_guest_command(&cfg);
    features_guest_stop(&feature_ctx);
    sync();
    reboot(LINUX_REBOOT_CMD_POWER_OFF);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
