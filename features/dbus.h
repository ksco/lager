#ifndef LAGER_FEATURES_DBUS_H
#define LAGER_FEATURES_DBUS_H

#include "registry.h"

void feature_dbus_host_resolve(struct host_ctx *ctx);
void feature_dbus_host_add_env(struct host_ctx *ctx);
void feature_dbus_guest_setup(struct guest_ctx *ctx);
void feature_dbus_guest_stop(struct guest_ctx *ctx);

#endif
