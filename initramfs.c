#define _GNU_SOURCE
#include "initramfs.h"

#include "log.h"
#include "modules.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct archive {
    FILE *file;
    size_t offset;
    unsigned int ino;
};

static void archive_write(struct archive *ar, const void *data, size_t size)
{
    if (fwrite(data, 1, size, ar->file) != size)
        die("writing initramfs: %s", strerror(errno));
    ar->offset += size;
}

static void archive_pad(struct archive *ar)
{
    static const char zeroes[4] = {0};
    size_t padding = (4 - (ar->offset & 3)) & 3;

    archive_write(ar, zeroes, padding);
}

static void archive_header(struct archive *ar, const char *name, mode_t mode, size_t size, unsigned int rdev_major,
                           unsigned int rdev_minor)
{
    char header[111];
    size_t name_size = strlen(name) + 1;
    int len;

    if (size > UINT32_MAX)
        die("initramfs member is too large: %s", name);
    len = snprintf(header, sizeof(header),
                   "070701%08x%08x%08x%08x%08x%08x%08x"
                   "%08x%08x%08x%08x%08x%08x",
                   ar->ino++, (unsigned int)mode, 0U, 0U, 1U, (unsigned int)time(NULL), (unsigned int)size, 0U, 0U,
                   rdev_major, rdev_minor, (unsigned int)name_size, 0U);
    if (len != 110)
        die("internal initramfs header error");
    archive_write(ar, header, 110);
    archive_write(ar, name, name_size);
    archive_pad(ar);
}

static void archive_directory(struct archive *ar, const char *name)
{
    archive_header(ar, name, S_IFDIR | 0755, 0, 0, 0);
}

static void archive_device(struct archive *ar, const char *name, mode_t mode, unsigned int major, unsigned int minor)
{
    archive_header(ar, name, S_IFCHR | mode, 0, major, minor);
}

static void archive_memory(struct archive *ar, const char *name, const void *data, size_t size, mode_t mode)
{
    archive_header(ar, name, S_IFREG | mode, size, 0, 0);
    archive_write(ar, data, size);
    archive_pad(ar);
}

static void archive_file(struct archive *ar, const char *name, const char *path, mode_t mode)
{
    struct stat st;
    FILE *input;
    unsigned char buffer[64 * 1024];
    size_t got;

    if (stat(path, &st) < 0)
        die("stat %s: %s", path, strerror(errno));
    if (!S_ISREG(st.st_mode))
        die("not a regular file: %s", path);
    input = fopen(path, "rb");
    if (!input)
        die("open %s: %s", path, strerror(errno));
    archive_header(ar, name, S_IFREG | mode, (size_t)st.st_size, 0, 0);
    while ((got = fread(buffer, 1, sizeof(buffer), input)) > 0)
        archive_write(ar, buffer, got);
    if (ferror(input))
        die("read %s: %s", path, strerror(errno));
    fclose(input);
    archive_pad(ar);
}

static void archive_finish(struct archive *ar)
{
    archive_header(ar, "TRAILER!!!", 0, 0, 0, 0);
    if (fclose(ar->file) < 0)
        die("close initramfs: %s", strerror(errno));
}

void make_initramfs(const char *path, const char *modules_dir, const struct bytebuf *config,
                    const char *compatible_gpu_module)
{
    static const char *transport_modules[] = {
        "virtio_pci_modern_dev",
        "virtio_pci_legacy_dev",
        "virtio_pci",
    };
    static const char *virtiofs_modules[] = {
        "fuse",
        "virtiofs",
    };
    struct archive ar = {0};
    char self[PATH_MAX];
    ssize_t self_len;
    size_t i;

    self_len = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (self_len < 0)
        die("readlink /proc/self/exe: %s", strerror(errno));
    self[self_len] = '\0';
    ar.file = fopen(path, "wb");
    if (!ar.file)
        die("create %s: %s", path, strerror(errno));
    ar.ino = 1;
    archive_directory(&ar, "dev");
    archive_directory(&ar, "lager");
    archive_directory(&ar, "modules");
    archive_device(&ar, "dev/console", 0600, 5, 1);
    archive_file(&ar, "init", self, 0755);
    archive_memory(&ar, "lager/config", config->data, config->len, 0600);
    for (i = 0; i < sizeof(transport_modules) / sizeof(transport_modules[0]); i++) {
        char *module = find_module(modules_dir, transport_modules[i]);
        const char *base = strrchr(module, '/');
        char *archive_name;

        base = base ? base + 1 : module;
        archive_name = xasprintf("modules/%s", base);
        archive_file(&ar, archive_name, module, 0644);
        free(archive_name);
        free(module);
    }
    for (i = 0; i < sizeof(virtiofs_modules) / sizeof(virtiofs_modules[0]); i++) {
        char *module = find_module(modules_dir, virtiofs_modules[i]);
        const char *base = strrchr(module, '/');
        char *archive_name;

        base = base ? base + 1 : module;
        archive_name = xasprintf("modules/%s", base);
        archive_file(&ar, archive_name, module, 0644);
        free(archive_name);
        free(module);
    }
    if (compatible_gpu_module) {
        char *module = find_module(modules_dir, "virtio_dma_buf");
        const char *base = strrchr(module, '/');
        char *archive_name;

        base = base ? base + 1 : module;
        archive_name = xasprintf("modules/%s", base);
        archive_file(&ar, archive_name, module, 0644);
        archive_file(&ar, "modules/virtio_gpu.ko", compatible_gpu_module, 0644);
        free(archive_name);
        free(module);
    }
    archive_finish(&ar);
}
