#ifndef LAGER_FEATURES_AUDIO_H
#define LAGER_FEATURES_AUDIO_H

#include "registry.h"

void feature_audio_host_resolve(struct host_ctx *ctx);
void feature_audio_host_add_env(struct host_ctx *ctx);
void feature_audio_host_add_qemu_options(struct host_ctx *ctx);
void feature_audio_guest_setup(struct guest_ctx *ctx);
void feature_audio_guest_stop(struct guest_ctx *ctx);

#endif
