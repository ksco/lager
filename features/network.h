#ifndef LAGER_FEATURES_NETWORK_H
#define LAGER_FEATURES_NETWORK_H

#include "registry.h"

void feature_network_host_add_qemu_options(struct host_ctx *ctx);
void feature_network_guest_setup(struct guest_ctx *ctx);

#endif
