#ifndef LAGER_INITRAMFS_H
#define LAGER_INITRAMFS_H

#include "utils.h"

void make_initramfs(const char *path, const char *modules_dir, const struct bytebuf *config,
                    const char *compatible_gpu_module);

#endif
