#define _GNU_SOURCE
#include "launcher.h"

#include "../misc/log.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define LAUNCHER_TITLE_MAX 80
#define LAUNCHER_TITLE_PREFIX "lager "
#define QEMU_DEVICE_HELP_MAX (256 * 1024)

static bool qemu_device_has_property(const char *program, const char *device, const char *property)
{
    struct bytebuf output = {0};
    char buf[4096];
    char *device_help;
    char *needle;
    int pipe_fds[2];
    int status = 0;
    pid_t pid;
    bool exited = false;
    bool found = false;

    if (pipe(pipe_fds) < 0) {
        warnx("cannot probe %s %s: pipe: %s", program, device, strerror(errno));
        return false;
    }
    pid = fork();
    if (pid < 0) {
        warnx("cannot probe %s %s: fork: %s", program, device, strerror(errno));
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return false;
    }
    if (pid == 0) {
        close(pipe_fds[0]);
        if (dup2(pipe_fds[1], STDOUT_FILENO) < 0 || dup2(pipe_fds[1], STDERR_FILENO) < 0)
            _exit(127);
        if (pipe_fds[1] > STDERR_FILENO)
            close(pipe_fds[1]);
        device_help = xasprintf("%s,help", device);
        execl(program, program, "-device", device_help, (char *)NULL);
        _exit(127);
    }
    close(pipe_fds[1]);
    for (;;) {
        ssize_t got = read(pipe_fds[0], buf, sizeof(buf));

        if (got < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (got == 0)
            break;
        if (output.len + (size_t)got <= QEMU_DEVICE_HELP_MAX)
            buf_append(&output, buf, (size_t)got);
    }
    close(pipe_fds[0]);
    for (;;) {
        pid_t got = waitpid(pid, &status, 0);

        if (got == pid) {
            exited = true;
            break;
        }
        if (got < 0 && errno != EINTR) {
            warnx("cannot probe %s %s: waitpid: %s", program, device, strerror(errno));
            break;
        }
    }
    if (exited && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        buf_append(&output, "", 1);
        needle = xasprintf("\n  %s=", property);
        found = strstr((char *)output.data, needle) != NULL;
        free(needle);
    }
    free(output.data);
    return found;
}

void launcher_resolve_programs(struct launcher_programs *programs)
{
    programs->qemu = find_program(NULL, "qemu-system-loongarch64", "/usr/bin/qemu-system-loongarch64");
    programs->virtiofsd = find_program(NULL, "virtiofsd", "/usr/libexec/virtiofsd");
    if (!programs->qemu)
        die("qemu-system-loongarch64 is required; install the system qemu "
            "package");
    if (!programs->virtiofsd)
        die("virtiofsd is required; install the system virtiofsd package");
    if (access("/dev/kvm", R_OK | W_OK) < 0)
        die("cannot access /dev/kvm: %s; add the current user to the kvm group", strerror(errno));
    programs->qemu_has_drm_native_context =
        qemu_device_has_property(programs->qemu, "virtio-gpu-gl-pci", "drm_native_context");
}

static char title_char(char ch)
{
    return (unsigned char)ch < ' ' || ch == ',' ? ' ' : ch;
}

char *launcher_command_title(char *const argv[])
{
    char *title;
    size_t wanted = 0;
    size_t limit;
    size_t out = 0;
    const size_t prefix_len = strlen(LAUNCHER_TITLE_PREFIX);

    if (!argv || !argv[0])
        return xstrdup("lager");
    wanted = prefix_len;
    for (size_t i = 0; argv[i]; i++) {
        if (i)
            wanted++;
        wanted += strlen(argv[i]);
    }
    limit = wanted > LAUNCHER_TITLE_MAX ? LAUNCHER_TITLE_MAX : wanted;
    title = xmalloc(limit + 1);
    while (out < prefix_len && out < limit) {
        title[out] = LAUNCHER_TITLE_PREFIX[out];
        out++;
    }
    for (size_t i = 0; argv[i] && out < limit; i++) {
        const char *arg = argv[i];

        if (i)
            title[out++] = ' ';
        while (*arg && out < limit)
            title[out++] = title_char(*arg++);
    }
    if (wanted > LAUNCHER_TITLE_MAX) {
        title[LAUNCHER_TITLE_MAX - 3] = '.';
        title[LAUNCHER_TITLE_MAX - 2] = '.';
        title[LAUNCHER_TITLE_MAX - 1] = '.';
    }
    title[out] = '\0';
    return title;
}

pid_t launcher_spawn(char *const argv[], bool quiet, const char *log_path)
{
    pid_t pid = fork();

    if (pid < 0)
        die("fork: %s", strerror(errno));
    if (pid == 0) {
        if (quiet)
            silence_output(log_path);
        execvp(argv[0], argv);
        fprintf(stderr, "lager: exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    return pid;
}

bool launcher_wait_for_socket(const char *path, pid_t child)
{
    int tries;

    for (tries = 0; tries < 100; tries++) {
        int status;

        if (path_exists(path))
            return true;
        if (waitpid(child, &status, WNOHANG) == child)
            return false;
        usleep(50000);
    }
    return false;
}

void launcher_terminate_child(pid_t *pid)
{
    int status;

    if (*pid <= 0)
        return;
    kill(*pid, SIGTERM);
    while (waitpid(*pid, &status, 0) < 0 && errno == EINTR)
        ;
    *pid = -1;
}

void launcher_build_virtiofsd_command(struct strvec *args, const char *program, const char *socket_path)
{
    vec_push_copy(args, program);
    vec_push_copy(args, "--socket-path");
    vec_push_copy(args, socket_path);
    vec_push_copy(args, "--shared-dir");
    vec_push_copy(args, "/");
    vec_push_copy(args, "--sandbox");
    vec_push_copy(args, "none");
}

void launcher_build_qemu_command(struct host_ctx *features, const char *program, const char *kernel,
                                 const char *initramfs, const char *rootfs_socket, int vcpus, unsigned long mem_mib,
                                 const char *title)
{
    struct config_header *header = features->header;
    char *kernel_args;
    char *chardev;
    char *fs_device;
    char *name;

    vec_push_copy(features->qemu, program);
    vec_push_copy(features->qemu, "-name");
    name = xasprintf("guest=%s", title);
    vec_push(features->qemu, name);
    vec_push_copy(features->qemu, "-machine");
    vec_push_copy(features->qemu, "virt,accel=kvm,memory-backend=mem,highmem-mmio=on");
    vec_push_copy(features->qemu, "-cpu");
    vec_push_copy(features->qemu, "host");
    vec_push_copy(features->qemu, "-nodefaults");
    vec_push_copy(features->qemu, "-monitor");
    vec_push_copy(features->qemu, "none");
    vec_push_copy(features->qemu, "-chardev");
    vec_push_copy(features->qemu, "stdio,id=console0,mux=on,signal=off");
    vec_push_copy(features->qemu, "-serial");
    vec_push_copy(features->qemu, "chardev:console0");
    vec_push_copy(features->qemu, "-no-reboot");
    vec_push_copy(features->qemu, "-smp");
    vec_push(features->qemu, xasprintf("%d", vcpus));
    vec_push_copy(features->qemu, "-m");
    vec_push(features->qemu, xasprintf("%luM", mem_mib));
    vec_push_copy(features->qemu, "-object");
    vec_push(features->qemu, xasprintf("memory-backend-memfd,id=mem,size=%luM,share=on", mem_mib));
    vec_push_copy(features->qemu, "-kernel");
    vec_push_copy(features->qemu, kernel);
    vec_push_copy(features->qemu, "-initrd");
    vec_push_copy(features->qemu, initramfs);
    vec_push_copy(features->qemu, "-append");
    kernel_args = xstrdup("console=ttyS0,115200 quiet loglevel=0 "
                          "panic=-1 reboot=t init=/init");
    if ((header->flags & CFG_WAYLAND) && header->resolution_width > 0) {
        char *with_resolution =
            xasprintf("%s video=Virtual-1:%ux%u@60", kernel_args, header->resolution_width, header->resolution_height);

        free(kernel_args);
        kernel_args = with_resolution;
    }
    vec_push(features->qemu, kernel_args);
    chardev = xasprintf("socket,id=fs0,path=%s", rootfs_socket);
    vec_push_copy(features->qemu, "-chardev");
    vec_push(features->qemu, chardev);
    fs_device = xstrdup("vhost-user-fs-pci,chardev=fs0,tag=" ROOT_TAG);
    vec_push_copy(features->qemu, "-device");
    vec_push(features->qemu, fs_device);
    vec_push_copy(features->qemu, "-device");
    vec_push_copy(features->qemu, "virtio-serial-pci");
    vec_push_copy(features->qemu, "-device");
    vec_push_copy(features->qemu, "virtconsole,chardev=console0");
    features_host_add_qemu_options(features);
}
