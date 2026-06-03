#ifndef LAGER_FEATURES_NOTIFICATIONS_H
#define LAGER_FEATURES_NOTIFICATIONS_H

#include "registry.h"

void feature_notifications_host_resolve(struct host_ctx *ctx);
void feature_notifications_guest_setup(struct guest_ctx *ctx);
void feature_notifications_guest_stop(struct guest_ctx *ctx);

#endif
