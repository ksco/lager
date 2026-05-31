#define _GNU_SOURCE
#include "network.h"

#include "../guest_net.h"

void feature_network_host_add_qemu_options(struct host_ctx *ctx)
{
    char *netdev;

    netdev = xstrdup("passt,id=net0");
    vec_push_copy(ctx->qemu, "-netdev");
    vec_push(ctx->qemu, netdev);
    vec_push_copy(ctx->qemu, "-device");
    vec_push_copy(ctx->qemu, "virtio-net-pci,netdev=net0");
}

void feature_network_guest_setup(struct guest_ctx *ctx)
{
    (void)ctx;
    setup_guest_net();
}
