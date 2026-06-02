#ifndef LAGER_H
#define LAGER_H

#include <stdint.h>

#define CFG_MAGIC "LAGER01"
#define CFG_VERSION 6
#define ROOT_TAG "lager-root"
#define MAX_CONFIG_SIZE (1024 * 1024)

enum {
    CFG_GPU = 1U << 0,
    CFG_NET = 1U << 1,
    CFG_X11 = 1U << 2,
    CFG_BINFMT = 1U << 4,
    CFG_AUDIO = 1U << 5,
    CFG_DBUS = 1U << 6,
    CFG_NOTIFICATIONS = 1U << 8,
    CFG_OPENBOX = 1U << 9,
};

struct config_header {
    char magic[8];
    uint32_t version;
    uint32_t uid;
    uint32_t gid;
    uint32_t argc;
    uint32_t envc;
    uint32_t flags;
    uint32_t resolution_width;
    uint32_t resolution_height;
    int64_t realtime_sec;
    uint32_t realtime_nsec;
};

#endif
