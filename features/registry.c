#define _GNU_SOURCE
#include "../features.h"

#include "../log.h"

#include "audio.h"
#include "box64.h"
#include "dbus.h"
#include "gpu.h"
#include "gpu_compat.h"
#include "network.h"
#include "notifications.h"
#include "openbox.h"
#include "power.h"
#include "x11.h"

enum feature_condition {
    FEATURE_ALWAYS,
    FEATURE_BOX64,
    FEATURE_GPU_COMPAT,
    FEATURE_X11,
};

struct feature {
    const char *name;
    uint32_t flag;
    uint32_t requires;
    enum feature_condition condition;
    void (*host_resolve)(struct host_ctx *ctx);
    void (*host_prepare)(struct host_ctx *ctx);
    void (*host_add_env)(struct host_ctx *ctx);
    void (*host_add_qemu_options)(struct host_ctx *ctx);
    void (*guest_setup)(struct guest_ctx *ctx);
    void (*guest_stop)(struct guest_ctx *ctx);
};

static bool feature_applies(const struct feature *feature,
                            const struct host_ctx *ctx)
{
    switch (feature->condition) {
    case FEATURE_ALWAYS:
        return true;
    case FEATURE_BOX64:
        return ctx->box64;
    case FEATURE_GPU_COMPAT:
        return ctx->opts->gpu_compat != FEATURE_OFF;
    case FEATURE_X11:
        return ctx->x11;
    }
    die("invalid feature condition");
}

static const struct feature features[] = {
    {
        .name = "networking",
        .flag = CFG_NET,
        .condition = FEATURE_ALWAYS,
        .host_add_qemu_options = feature_network_host_add_qemu_options,
        .guest_setup = feature_network_guest_setup,
    },
    {
        .name = "dbus",
        .flag = CFG_DBUS,
        .condition = FEATURE_ALWAYS,
        .host_resolve = feature_dbus_host_resolve,
        .host_add_env = feature_dbus_host_add_env,
        .guest_setup = feature_dbus_guest_setup,
        .guest_stop = feature_dbus_guest_stop,
    },
    {
        .name = "power services",
        .flag = CFG_POWER,
        .requires = CFG_DBUS,
        .condition = FEATURE_ALWAYS,
        .guest_setup = feature_power_guest_setup,
        .guest_stop = feature_power_guest_stop,
    },
    {
        .name = "audio",
        .flag = CFG_AUDIO,
        .condition = FEATURE_ALWAYS,
        .host_resolve = feature_audio_host_resolve,
        .host_add_env = feature_audio_host_add_env,
        .host_add_qemu_options = feature_audio_host_add_qemu_options,
        .guest_setup = feature_audio_guest_setup,
        .guest_stop = feature_audio_guest_stop,
    },
    {
        .name = "gpu",
        .flag = CFG_GPU,
        .condition = FEATURE_ALWAYS,
        .host_add_env = feature_gpu_host_add_env,
        .host_add_qemu_options = feature_gpu_host_add_qemu_options,
        .guest_setup = feature_gpu_guest_setup,
    },
    {
        .name = "gpu compat",
        .condition = FEATURE_GPU_COMPAT,
        .host_prepare = feature_gpu_compat_host_prepare,
    },
    {
        .name = "box64 binfmt",
        .flag = CFG_BINFMT,
        .condition = FEATURE_BOX64,
        .host_resolve = feature_box64_host_resolve,
        .guest_setup = feature_box64_guest_setup,
    },
    {
        .name = "x11",
        .flag = CFG_X11,
        .requires = CFG_GPU,
        .condition = FEATURE_X11,
        .host_resolve = feature_x11_host_resolve,
        .host_add_env = feature_x11_host_add_env,
        .host_add_qemu_options = feature_x11_host_add_qemu_options,
        .guest_setup = feature_x11_guest_setup,
        .guest_stop = feature_x11_guest_stop,
    },
    {
        .name = "notifications",
        .flag = CFG_NOTIFICATIONS,
        .requires = CFG_DBUS | CFG_X11,
        .condition = FEATURE_X11,
        .host_resolve = feature_notifications_host_resolve,
        .guest_setup = feature_notifications_guest_setup,
        .guest_stop = feature_notifications_guest_stop,
    },
    {
        .name = "openbox",
        .flag = CFG_OPENBOX,
        .requires = CFG_X11,
        .condition = FEATURE_X11,
        .host_resolve = feature_openbox_host_resolve,
        .guest_setup = feature_openbox_guest_setup,
        .guest_stop = feature_openbox_guest_stop,
    },
};

#define FEATURE_COUNT (sizeof(features) / sizeof(features[0]))

void features_host_resolve(struct host_ctx *ctx)
{
    size_t i;

    ctx->header->flags = 0;
    for (i = 0; i < FEATURE_COUNT; i++) {
        if (features[i].host_resolve)
            features[i].host_resolve(ctx);
    }
    for (i = 0; i < FEATURE_COUNT; i++) {
        if (feature_applies(&features[i], ctx))
            ctx->header->flags |= features[i].flag;
    }
    for (i = 0; i < FEATURE_COUNT; i++) {
        if ((ctx->header->flags & features[i].flag) &&
            (ctx->header->flags & features[i].requires) != features[i].requires)
            die("%s requires additional active features", features[i].name);
    }
}

void features_host_prepare(struct host_ctx *ctx)
{
    size_t i;

    for (i = 0; i < FEATURE_COUNT; i++) {
        if (feature_applies(&features[i], ctx) && features[i].host_prepare)
            features[i].host_prepare(ctx);
    }
}

void features_host_add_env(struct host_ctx *ctx)
{
    size_t i;

    for (i = 0; i < FEATURE_COUNT; i++) {
        if (feature_applies(&features[i], ctx) && features[i].host_add_env)
            features[i].host_add_env(ctx);
    }
}

void features_host_add_qemu_options(struct host_ctx *ctx)
{
    size_t i;

    for (i = 0; i < FEATURE_COUNT; i++) {
        if (feature_applies(&features[i], ctx) &&
            features[i].host_add_qemu_options)
            features[i].host_add_qemu_options(ctx);
    }
}

void features_guest_setup(struct guest_ctx *ctx)
{
    size_t i;

    for (i = 0; i < FEATURE_COUNT; i++) {
        if ((ctx->cfg->header.flags & features[i].flag) &&
            features[i].guest_setup)
            features[i].guest_setup(ctx);
    }
}

void features_guest_stop(struct guest_ctx *ctx)
{
    size_t i = FEATURE_COUNT;

    while (i--) {
        if ((ctx->cfg->header.flags & features[i].flag) &&
            features[i].guest_stop)
            features[i].guest_stop(ctx);
    }
}
