#ifndef LAGER_FEATURES_H
#define LAGER_FEATURES_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "guest_config.h"
#include "lager.h"
#include "utils.h"

struct host_ctx {
    struct lager_config *opts;
    struct config_header *header;
    struct strvec *env;
    struct strvec *qemu;
    const char *runtime;
    const char *qemu_program;
    const char *modules_dir;
    const char *box64;
    unsigned long gpu_hostmem_mib;
    bool qemu_has_drm_native_context;
    bool qemu_has_rutabaga_gpu;
    bool guest_x11;
    char *compatible_gpu_module;
    char *audio_backend;
};

struct guest_ctx {
    struct guest_config *cfg;
    int log_fd;
};

void features_host_resolve(struct host_ctx *ctx);
void features_host_prepare(struct host_ctx *ctx);
void features_host_add_env(struct host_ctx *ctx);
void features_host_add_qemu_options(struct host_ctx *ctx);
void features_guest_setup(struct guest_ctx *ctx);
void features_guest_stop(struct guest_ctx *ctx);

#endif
