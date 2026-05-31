#ifndef LAGER_FEATURES_GPU_COMPAT_H
#define LAGER_FEATURES_GPU_COMPAT_H

#include <stdbool.h>

struct host_ctx;

bool gpu_compat_needed(const char *modules_dir);
char *gpu_compat_prepare(const char *runtime, const char *modules_dir);
void feature_gpu_compat_host_prepare(struct host_ctx *ctx);

#endif
