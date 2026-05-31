#define _GNU_SOURCE
#include "launcher.h"

#include "log.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

void launcher_resolve_programs(struct launcher_programs *programs)
{
    programs->qemu = find_program(NULL, "qemu-system-loongarch64",
                                  "/usr/bin/qemu-system-loongarch64");
    programs->virtiofsd =
        find_program(NULL, "virtiofsd", "/usr/libexec/virtiofsd");
    if (!programs->qemu)
        die("qemu-system-loongarch64 is required; install the system qemu "
            "package");
    if (!programs->virtiofsd)
        die("virtiofsd is required; install the system virtiofsd package");
    if (access("/dev/kvm", R_OK | W_OK) < 0)
        die("cannot access /dev/kvm: %s; add the current user to the kvm group",
            strerror(errno));
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

void launcher_build_virtiofsd_command(struct strvec *args, const char *program,
                                      const char *socket_path)
{
    vec_push_copy(args, program);
    vec_push_copy(args, "--socket-path");
    vec_push_copy(args, socket_path);
    vec_push_copy(args, "--shared-dir");
    vec_push_copy(args, "/");
    vec_push_copy(args, "--sandbox");
    vec_push_copy(args, "none");
}

void launcher_build_qemu_command(struct host_ctx *features, const char *program,
                                 const char *kernel, const char *initramfs,
                                 const char *rootfs_socket, int vcpus,
                                 unsigned long mem_mib)
{
    struct config_header *header = features->header;
    char *kernel_args;
    char *chardev;
    char *fs_device;

    vec_push_copy(features->qemu, program);
    vec_push_copy(features->qemu, "-machine");
    vec_push_copy(features->qemu,
                  "virt,accel=kvm,memory-backend=mem,highmem-mmio=on");
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
    vec_push(
        features->qemu,
        xasprintf("memory-backend-memfd,id=mem,size=%luM,share=on", mem_mib));
    vec_push_copy(features->qemu, "-kernel");
    vec_push_copy(features->qemu, kernel);
    vec_push_copy(features->qemu, "-initrd");
    vec_push_copy(features->qemu, initramfs);
    vec_push_copy(features->qemu, "-append");
    kernel_args = xstrdup("console=ttyS0,115200 quiet loglevel=0 "
                          "panic=-1 reboot=t init=/init");
    if ((header->flags & CFG_X11) && header->resolution_width > 0) {
        char *with_resolution =
            xasprintf("%s video=Virtual-1:%ux%u@60", kernel_args,
                      header->resolution_width, header->resolution_height);

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
