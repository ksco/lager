#ifndef LAGER_FEATURES_WAYLAND_H
#define LAGER_FEATURES_WAYLAND_H

#include "registry.h"

void feature_wayland_host_resolve(struct host_ctx *ctx);
void feature_wayland_host_add_env(struct host_ctx *ctx);
void feature_wayland_host_add_qemu_options(struct host_ctx *ctx);
void feature_wayland_guest_setup(struct guest_ctx *ctx);
void feature_wayland_guest_stop(struct guest_ctx *ctx);

#endif
