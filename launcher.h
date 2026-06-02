#ifndef LAGER_LAUNCHER_H
#define LAGER_LAUNCHER_H

#include "features.h"
#include "utils.h"

#include <stdbool.h>
#include <sys/types.h>

struct launcher_programs {
    char *qemu;
    char *virtiofsd;
    bool qemu_has_drm_native_context;
    bool qemu_has_rutabaga_gpu;
};

void launcher_resolve_programs(struct launcher_programs *programs);
char *launcher_command_title(char *const argv[]);
pid_t launcher_spawn(char *const argv[], bool quiet, const char *log_path);
bool launcher_wait_for_socket(const char *path, pid_t child);
void launcher_terminate_child(pid_t *pid);

void launcher_build_virtiofsd_command(struct strvec *args, const char *program, const char *socket_path);
void launcher_build_qemu_command(struct host_ctx *features, const char *program, const char *kernel,
                                 const char *initramfs, const char *rootfs_socket, int vcpus, unsigned long mem_mib,
                                 const char *title);

#endif
