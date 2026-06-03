#ifndef LAGER_FEATURES_GPU_H
#define LAGER_FEATURES_GPU_H

#include "registry.h"

void feature_gpu_host_add_env(struct host_ctx *ctx);
void feature_gpu_host_add_qemu_options(struct host_ctx *ctx);
void feature_gpu_guest_setup(struct guest_ctx *ctx);

#endif
