#define _GNU_SOURCE
#include "gpu.h"

#include "../log.h"

#include <errno.h>
#include <glob.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define QEMU_RUTABAGA_GPU_DEVICE "virtio-gpu-rutabaga-pci"

static enum gpu_backend active_gpu_backend(const struct host_ctx *ctx)
{
    switch (ctx->opts->gpu_backend) {
    case GPU_BACKEND_AUTO:
        return ctx->qemu_has_rutabaga_gpu ? GPU_BACKEND_RUTABAGA : GPU_BACKEND_VIRGL;
    case GPU_BACKEND_RUTABAGA:
        if (!ctx->qemu_has_rutabaga_gpu)
            die("gpu_backend is rutabaga, but %s does not support %s", ctx->qemu_program, QEMU_RUTABAGA_GPU_DEVICE);
        return GPU_BACKEND_RUTABAGA;
    case GPU_BACKEND_VIRGL:
        return GPU_BACKEND_VIRGL;
    }
    die("invalid GPU backend");
}

static const char *gfxstream_vk_icd(void)
{
    static const char *candidates[] = {
        "/usr/share/vulkan/icd.d/gfxstream_vk_icd.loongarch64.json",
        "/usr/share/vulkan/icd.d/gfxstream_vk_icd.json",
        "/usr/share/vulkan/icd.d/gfxstream_vk_devenv_icd.loongarch64.json",
    };
    size_t i;

    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++)
        if (path_exists(candidates[i]))
            return candidates[i];
    return NULL;
}

static char *host_wayland_socket_path(void)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *display = getenv("WAYLAND_DISPLAY");

    if (display && display[0] == '/')
        return xstrdup(display);
    if (!display || !*display)
        display = "wayland-0";
    if (!runtime || !*runtime)
        return NULL;
    return xasprintf("%s/%s", runtime, display);
}

void feature_gpu_host_add_env(struct host_ctx *ctx)
{
    char *assignment;
    const char *icd;

    if (active_gpu_backend(ctx) == GPU_BACKEND_RUTABAGA) {
        icd = gfxstream_vk_icd();
        env_set(ctx->env, "MESA_LOADER_DRIVER_OVERRIDE=zink");
        if (icd) {
            assignment = xasprintf("VK_DRIVER_FILES=%s", icd);
            env_set(ctx->env, assignment);
            free(assignment);
            assignment = xasprintf("VK_ICD_FILENAMES=%s", icd);
            env_set(ctx->env, assignment);
            free(assignment);
        } else {
            warnx("rutabaga selected but no gfxstream Vulkan ICD was found; "
                  "guest GL/Vulkan needs Mesa gfxstream support");
        }
        env_set(ctx->env, "WAYLAND_DISPLAY=wayland-0");
        env_set(ctx->env, "XDG_SESSION_TYPE=wayland");
        env_set(ctx->env, "GDK_BACKEND=wayland,x11");
        env_set(ctx->env, "QT_QPA_PLATFORM=wayland;xcb");
        env_set(ctx->env, "SDL_VIDEODRIVER=wayland");
    } else {
        env_set(ctx->env, "MESA_LOADER_DRIVER_OVERRIDE=zink");
        env_set(ctx->env, "LIBGL_KOPPER_DRI2=true");
        env_set(ctx->env, "VK_DRIVER_FILES=/usr/share/vulkan/icd.d/"
                          "virtio_icd.loongarch64.json");
        env_set(ctx->env, "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/"
                          "virtio_icd.loongarch64.json");
    }
    assignment = xasprintf("HK_SYSMEM=%lu", ctx->gpu_hostmem_mib << 20);
    env_set(ctx->env, assignment);
    free(assignment);
}

static void add_virgl_qemu_options(struct host_ctx *ctx)
{
    char *res_suffix = NULL;
    const char *native_context_suffix = ctx->qemu_has_drm_native_context ? ",drm_native_context=on" : "";
    char *device;

    if ((ctx->header->flags & CFG_X11) && ctx->header->resolution_width > 0)
        res_suffix = xasprintf(",xres=%u,yres=%u", ctx->header->resolution_width, ctx->header->resolution_height);
    device = xasprintf("virtio-gpu-gl-pci,blob=on,venus=on,"
                       "hostmem=%luM,max_hostmem=%luM%s%s",
                       ctx->gpu_hostmem_mib, ctx->gpu_hostmem_mib, native_context_suffix, res_suffix ? res_suffix : "");
    free(res_suffix);
    vec_push_copy(ctx->qemu, "-display");
    vec_push_copy(ctx->qemu, ctx->header->flags & CFG_X11 ? "gtk,gl=on,zoom-to-fit=off" : "egl-headless,gl=on");
    vec_push_copy(ctx->qemu, "-device");
    vec_push(ctx->qemu, device);
}

static void add_rutabaga_qemu_options(struct host_ctx *ctx)
{
    char *device;
    char *socket = host_wayland_socket_path();

    device = xasprintf("%s,blob=on,hostmem=%luM,max_hostmem=%luM,cross-domain=on,wsi=surfaceless",
                       QEMU_RUTABAGA_GPU_DEVICE, ctx->gpu_hostmem_mib, ctx->gpu_hostmem_mib);
    if (socket && path_exists(socket)) {
        if (strchr(socket, ',')) {
            warnx("cannot pass Wayland socket path containing a comma to QEMU: %s", socket);
        } else {
            char *with_socket = xasprintf("%s,wayland-socket-path=%s", device, socket);

            free(device);
            device = with_socket;
        }
    } else {
        warnx("rutabaga cross-domain selected but no host Wayland socket was found");
    }
    free(socket);
    vec_push_copy(ctx->qemu, "-display");
    vec_push_copy(ctx->qemu, ctx->header->flags & CFG_X11 ? "gtk,gl=on,zoom-to-fit=off" : "egl-headless,gl=on");
    vec_push_copy(ctx->qemu, "-device");
    vec_push(ctx->qemu, device);
}

void feature_gpu_host_add_qemu_options(struct host_ctx *ctx)
{
    if (active_gpu_backend(ctx) == GPU_BACKEND_RUTABAGA)
        add_rutabaga_qemu_options(ctx);
    else
        add_virgl_qemu_options(ctx);
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
