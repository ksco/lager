#define _GNU_SOURCE
#include "gpu.h"

#include "../misc/log.h"

#include <errno.h>
#include <glob.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool gpu_uses_venus_zink(const struct host_ctx *ctx)
{
    return !ctx->qemu_has_drm_native_context;
}

void feature_gpu_host_add_env(struct host_ctx *ctx)
{
    char *assignment;

    if (!gpu_uses_venus_zink(ctx)) {
        env_unset(ctx->env, "MESA_LOADER_DRIVER_OVERRIDE");
        env_unset(ctx->env, "LIBGL_KOPPER_DRI2");
        env_unset(ctx->env, "VK_DRIVER_FILES");
        env_unset(ctx->env, "VK_ICD_FILENAMES");
        env_unset(ctx->env, "HK_SYSMEM");
        return;
    }

    env_set(ctx->env, "MESA_LOADER_DRIVER_OVERRIDE=zink");
    env_set(ctx->env, "LIBGL_KOPPER_DRI2=true");
    env_set(ctx->env, "VK_DRIVER_FILES=/usr/share/vulkan/icd.d/"
                      "virtio_icd.loongarch64.json");
    env_set(ctx->env, "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/"
                      "virtio_icd.loongarch64.json");
    assignment = xasprintf("HK_SYSMEM=%lu", ctx->gpu_hostmem_mib << 20);
    env_set(ctx->env, assignment);
    free(assignment);
}

void feature_gpu_host_add_qemu_options(struct host_ctx *ctx)
{
    char *res_suffix = NULL;
    const char *context_suffix = gpu_uses_venus_zink(ctx) ? ",venus=on" : ",drm_native_context=on";
    char *device;

    if ((ctx->header->flags & CFG_WAYLAND) && ctx->header->resolution_width > 0)
        res_suffix = xasprintf(",xres=%u,yres=%u", ctx->header->resolution_width, ctx->header->resolution_height);
    device = xasprintf("virtio-gpu-gl-pci,blob=on%s,"
                       "hostmem=%luM,max_hostmem=%luM%s",
                       context_suffix, ctx->gpu_hostmem_mib, ctx->gpu_hostmem_mib, res_suffix ? res_suffix : "");
    free(res_suffix);
    vec_push_copy(ctx->qemu, "-display");
    vec_push_copy(ctx->qemu, ctx->header->flags & CFG_WAYLAND ? "sdl,gl=on" : "egl-headless,gl=on");
    vec_push_copy(ctx->qemu, "-device");
    vec_push(ctx->qemu, device);
}

static void permit_guest_render_nodes(void)
{
    glob_t nodes = {0};
    size_t i;

    if (glob("/dev/dri/renderD*", 0, NULL, &nodes) != 0)
        return;
    for (i = 0; i < nodes.gl_pathc; i++)
        if (chmod(nodes.gl_pathv[i], 0666) < 0)
            warnx("cannot grant access to %s: %s", nodes.gl_pathv[i], strerror(errno));
    globfree(&nodes);
}

void feature_gpu_guest_setup(struct guest_ctx *ctx)
{
    (void)ctx;
    permit_guest_render_nodes();
}
