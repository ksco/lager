#define _GNU_SOURCE
#include "box64.h"

#include "../misc/log.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>

void feature_box64_host_resolve(struct host_ctx *ctx)
{
    ctx->box64 = find_in_path("box64");
    if (!ctx->box64)
        warnx("box64 was not found; automatic binfmt registration is disabled");
}

static void buf_append_hex_escaped(struct bytebuf *buf, const unsigned char *data, size_t size)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < size; i++) {
        char escaped[] = {'\\', 'x', hex[data[i] >> 4], hex[data[i] & 0xf]};

        buf_append(buf, escaped, sizeof(escaped));
    }
}

static void register_binfmt(const char *name, const unsigned char *magic, size_t magic_len, const unsigned char *mask,
                            size_t mask_len, const char *interpreter)
{
    struct bytebuf rule = {0};
    int fd;
    char *path;

    path = xasprintf("/proc/sys/fs/binfmt_misc/%s", name);
    fd = open(path, O_WRONLY | O_CLOEXEC);
    free(path);
    if (fd >= 0) {
        write_all(fd, "-1", 2);
        close(fd);
    }
    buf_append(&rule, ":", 1);
    buf_append(&rule, name, strlen(name));
    buf_append(&rule, ":M::", 4);
    buf_append_hex_escaped(&rule, magic, magic_len);
    buf_append(&rule, ":", 1);
    buf_append_hex_escaped(&rule, mask, mask_len);
    buf_append(&rule, ":", 1);
    buf_append(&rule, interpreter, strlen(interpreter));
    buf_append(&rule, ":CF", 3);
    fd = open("/proc/sys/fs/binfmt_misc/register", O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        die("open binfmt_misc register: %s", strerror(errno));
    write_all(fd, rule.data, rule.len);
    close(fd);
    free(rule.data);
}

static void setup_box64(const char *box64)
{
    static const unsigned char magic64[] = {
        0x7f, 'E',  'L',  'F',  0x02, 0x01, 0x01, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x3e, 0x00,
    };
    static const unsigned char mask64[] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00,
        0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe, 0xff, 0xff, 0xff,
    };
    static const unsigned char magic32[] = {
        0x7f, 'E',  'L',  'F',  0x01, 0x01, 0x01, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x03, 0x00,
    };
    static const unsigned char mask32[] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe, 0xff, 0xff, 0xff,
    };

    if (!box64[0])
        return;
    mkdir_ok("/proc/sys/fs/binfmt_misc", 0755);
    if (mount("binfmt_misc", "/proc/sys/fs/binfmt_misc", "binfmt_misc", 0, NULL) < 0 && errno != EBUSY)
        die("mount binfmt_misc: %s", strerror(errno));
    register_binfmt("BOX64", magic64, sizeof(magic64), mask64, sizeof(mask64), box64);
    register_binfmt("BOX32", magic32, sizeof(magic32), mask32, sizeof(mask32), box64);
}

void feature_box64_guest_setup(struct guest_ctx *ctx)
{
    setup_box64(ctx->cfg->box64);
}
