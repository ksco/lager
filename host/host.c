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
#include "../misc/config.h"
#include "../features/registry.h"
#include "../guest/config.h"
#include "host.h"
#include "initramfs.h"
#include "launcher.h"
#include "../misc/log.h"
#include "../misc/utils.h"

#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static pid_t host_qemu_pid = -1;
static pid_t host_virtiofsd_pid = -1;
static int configure_cpus(void)
{
    cpu_set_t set;
    int count;

    if (sched_getaffinity(0, sizeof(set), &set) < 0)
        die("sched_getaffinity: %s", strerror(errno));
    count = CPU_COUNT(&set);
    return count > 0 ? count : 1;
}

static unsigned long host_ram_mib(void)
{
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);

    if (pages < 0 || page_size < 0)
        return 4096;
    return (unsigned long)(((unsigned long long)pages * page_size) >> 20);
}

static unsigned long round_down_power_of_two(unsigned long value)
{
    unsigned long result = 1;

    while (result <= value / 2)
        result <<= 1;
    return result;
}

static void parse_resolution(const char *resolution, uint32_t *width, uint32_t *height)
{
    char *end;
    unsigned long w, h;

    if (!resolution || !*resolution) {
        *width = 0;
        *height = 0;
        return;
    }
    w = strtoul(resolution, &end, 10);
    if (end == resolution || *end != 'x' || !end[1])
        die("invalid resolution format: %s (expected WIDTHxHEIGHT)", resolution);
    h = strtoul(end + 1, &end, 10);
    if (*end || h == 0 || w == 0 || w > 16384 || h > 16384)
        die("invalid resolution format: %s (expected WIDTHxHEIGHT)", resolution);
    *width = (uint32_t)w;
    *height = (uint32_t)h;
}

static char *infer_modules_dir(const char *kernel)
{
    const char *base = strrchr(kernel, '/');

    base = base ? base + 1 : kernel;
    if (strncmp(base, "vmlinuz-", 8) != 0)
        die("modules_dir is required when the kernel is not named "
            "vmlinuz-<release>");
    return xasprintf("/usr/lib/modules/%s", base + 8);
}

static char *default_kernel(void)
{
    struct utsname uts;
    char *candidate;
    char *marker;
    glob_t matches = {0};

    if (uname(&uts) == 0) {
        char *release = xstrdup(uts.release);

        marker = strstr(release, "-16k");
        if (marker) {
            *marker = '\0';
            candidate = xasprintf("/boot/vmlinuz-%s-4k%s", release, marker + 4);
        } else if (strstr(release, "-4k")) {
            candidate = xasprintf("/boot/vmlinuz-%s", release);
        } else {
            candidate = NULL;
        }
        free(release);
        if (candidate && path_exists(candidate))
            return candidate;
        free(candidate);
    }
    if (glob("/boot/vmlinuz-*-4k", 0, NULL, &matches) == 0 && matches.gl_pathc > 0) {
        candidate = xstrdup(matches.gl_pathv[matches.gl_pathc - 1]);
        globfree(&matches);
        return candidate;
    }
    globfree(&matches);
    return NULL;
}

static void host_signal(int signal_number)
{
    if (host_qemu_pid > 0)
        kill(host_qemu_pid, signal_number);
}

static void cleanup_runtime(const char *runtime)
{
    char *path;

    launcher_terminate_child(&host_virtiofsd_pid);
    path = xasprintf("%s/rootfs.sock", runtime);
    unlink(path);
    free(path);
    path = xasprintf("%s/rootfs.sock.pid", runtime);
    unlink(path);
    free(path);
    path = xasprintf("%s/initramfs.cpio", runtime);
    unlink(path);
    free(path);
    path = xasprintf("%s/virtio-gpu.ko", runtime);
    unlink(path);
    free(path);
    rmdir(runtime);
}

static void raise_nofile_limit(void)
{
    struct rlimit limit;

    if (getrlimit(RLIMIT_NOFILE, &limit) == 0) {
        limit.rlim_cur = limit.rlim_max;
        setrlimit(RLIMIT_NOFILE, &limit);
    }
}

int host_main(int argc, char **argv)
{
    struct lager_config opts;
    struct config_header header = {0};
    struct host_ctx feature_ctx = {0};
    struct strvec env;
    struct bytebuf config;
    struct strvec qemu = {0};
    struct strvec virtiofsd = {0};
    struct launcher_programs programs = {0};
    struct timespec realtime;
    int vcpus;
    int status;
    int qemu_stderr_fd;
    int qemu_log_fd;
    unsigned long ram;
    unsigned long mem_mib;
    unsigned long gpu_hostmem_mib;
    char *kernel;
    char *modules_dir;
    char *log_path;
    char *qemu_title;
    char runtime[] = "/tmp/lager-XXXXXX";
    char *initramfs;
    char *rootfs_socket;
    char *compatible_gpu_module = NULL;
    char *assignment;
    char cwd[PATH_MAX];
    const char *workdir;
    enum display_type display = DISPLAY_WAYLAND;
    int config_status;

    config_status = handle_lager_config_cli(argc, argv);
    if (config_status >= 0)
        return config_status;
    load_lager_config(&opts);
    log_path = default_log_path();
    if (argc > 1 && !strcmp(argv[1], "-headless")) {
        if (argc < 4 || strcmp(argv[2], "--"))
            die("usage: lager -headless -- COMMAND [ARGS...]");
        display = DISPLAY_NONE;
        argc -= 2;
        argv += 2;
    }
    if (argc < 2)
        die("no command specified; usage: lager COMMAND [ARGS...]");
    qemu_title = launcher_command_title(&argv[1]);
    vcpus = configure_cpus();
    ram = host_ram_mib();
    mem_mib = ram * 4 / 5;
    gpu_hostmem_mib = ram / 8;
    if (gpu_hostmem_mib < 256)
        gpu_hostmem_mib = 256;
    if (gpu_hostmem_mib > 8192)
        gpu_hostmem_mib = 8192;
    gpu_hostmem_mib = round_down_power_of_two(gpu_hostmem_mib);
    kernel = opts.kernel ? xstrdup(opts.kernel) : default_kernel();
    if (!kernel || !path_exists(kernel))
        die("cannot find a 4 KiB system kernel; set \"kernel\" in config.json");
    modules_dir = opts.modules_dir ? xstrdup(opts.modules_dir) : infer_modules_dir(kernel);
    if (!path_exists(modules_dir))
        die("matching modules directory does not exist: %s", modules_dir);
    launcher_resolve_programs(&programs);
    feature_ctx.opts = &opts;
    feature_ctx.header = &header;
    feature_ctx.qemu = &qemu;
    feature_ctx.runtime = runtime;
    feature_ctx.modules_dir = modules_dir;
    feature_ctx.gpu_hostmem_mib = gpu_hostmem_mib;
    feature_ctx.qemu_has_drm_native_context = programs.qemu_has_drm_native_context;
    feature_ctx.display = display;
    features_host_resolve(&feature_ctx);
    if (!mkdtemp(runtime))
        die("mkdtemp: %s", strerror(errno));
    initramfs = xasprintf("%s/initramfs.cpio", runtime);
    rootfs_socket = xasprintf("%s/rootfs.sock", runtime);
    features_host_prepare(&feature_ctx);
    compatible_gpu_module = feature_ctx.compatible_gpu_module;

    env = copy_host_env();
    feature_ctx.env = &env;
    env_set(&env, "DISPLAY=");
    env_set(&env, "DBUS_SESSION_BUS_ADDRESS=");
    env_set(&env, "PULSE_SERVER=");
    assignment = xasprintf("XDG_RUNTIME_DIR=/run/user/%lu", (unsigned long)getuid());
    env_set(&env, assignment);
    free(assignment);
    features_host_add_env(&feature_ctx);
    if (getcwd(cwd, sizeof(cwd)))
        workdir = cwd;
    else
        workdir = "/";
    memcpy(header.magic, CFG_MAGIC, sizeof(header.magic));
    header.version = CFG_VERSION;
    header.uid = (uint32_t)getuid();
    header.gid = (uint32_t)getgid();
    header.argc = (uint32_t)(argc - 1);
    header.envc = (uint32_t)env.len;
    parse_resolution(opts.resolution, &header.resolution_width, &header.resolution_height);
    if (clock_gettime(CLOCK_REALTIME, &realtime) < 0)
        die("clock_gettime: %s", strerror(errno));
    header.realtime_sec = realtime.tv_sec;
    header.realtime_nsec = (uint32_t)realtime.tv_nsec;
    config = make_guest_config(&header, workdir, feature_ctx.box64, log_path, &argv[1], &env);
    make_initramfs(initramfs, modules_dir, &config, compatible_gpu_module);

    launcher_build_virtiofsd_command(&virtiofsd, programs.virtiofsd, rootfs_socket);
    launcher_build_qemu_command(&feature_ctx, programs.qemu, kernel, initramfs, rootfs_socket, vcpus, mem_mib,
                                qemu_title);

    raise_nofile_limit();
    prepare_log(log_path);
    signal(SIGINT, host_signal);
    signal(SIGTERM, host_signal);
    signal(SIGHUP, host_signal);
    host_virtiofsd_pid = launcher_spawn(virtiofsd.items, true, log_path);
    if (!launcher_wait_for_socket(rootfs_socket, host_virtiofsd_pid)) {
        cleanup_runtime(runtime);
        die("virtiofsd failed to create %s", rootfs_socket);
    }
    qemu_log_fd = open_log(log_path);
    if (qemu_log_fd < 0) {
        cleanup_runtime(runtime);
        die("open %s: %s", log_path, strerror(errno));
    }
    host_qemu_pid = spawn_stderr_tee(qemu.items, &qemu_stderr_fd);
    status = wait_for_stderr_tee(host_qemu_pid, qemu_stderr_fd, qemu_log_fd, "qemu");
    close(qemu_log_fd);
    host_qemu_pid = -1;
    cleanup_runtime(runtime);
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 128 + WTERMSIG(status);
}
