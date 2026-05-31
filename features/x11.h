#ifndef LAGER_FEATURES_X11_H
#define LAGER_FEATURES_X11_H

#include "../features.h"

void feature_x11_host_resolve(struct host_ctx *ctx);
void feature_x11_host_add_env(struct host_ctx *ctx);
void feature_x11_host_add_qemu_options(struct host_ctx *ctx);
void feature_x11_guest_setup(struct guest_ctx *ctx);
void feature_x11_guest_stop(struct guest_ctx *ctx);

#endif
