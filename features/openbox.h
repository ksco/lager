#ifndef LAGER_FEATURES_OPENBOX_H
#define LAGER_FEATURES_OPENBOX_H

#include "../features.h"

void feature_openbox_host_resolve(struct host_ctx *ctx);
void feature_openbox_guest_setup(struct guest_ctx *ctx);
void feature_openbox_guest_stop(struct guest_ctx *ctx);

#endif
