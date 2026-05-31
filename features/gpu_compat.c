#define _GNU_SOURCE
#include "gpu_compat.h"

#include "../features.h"
#include "../log.h"
#include "../modules.h"
#include "../utils.h"

#include <arpa/inet.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_MODULE_SIZE (64 * 1024 * 1024)
#define MODULE_SIGNATURE_MAGIC "~Module signature appended~\n"

struct module_signature {
    uint8_t algo;
    uint8_t hash;
    uint8_t id_type;
    uint8_t signer_len;
    uint8_t key_id_len;
    uint8_t pad[3];
    uint32_t sig_len;
} __attribute__((packed));

static bool range_ok(size_t size, size_t offset, size_t length)
{
    return offset <= size && length <= size - offset;
}

static uint32_t read_le32(const void *data)
{
    const unsigned char *bytes = data;

    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static void write_le32(void *data, uint32_t value)
{
    unsigned char *bytes = data;

    bytes[0] = value;
    bytes[1] = value >> 8;
    bytes[2] = value >> 16;
    bytes[3] = value >> 24;
}

static const char *elf_string(const unsigned char *data, size_t size,
                              const Elf64_Shdr *strings, uint32_t offset)
{
    const char *text;
    size_t available;

    if (offset >= strings->sh_size ||
        !range_ok(size, strings->sh_offset, strings->sh_size))
        die("invalid GPU module string table");
    text = (const char *)data + strings->sh_offset + offset;
    available = (size_t)strings->sh_size - offset;
    if (!memchr(text, '\0', available))
        die("unterminated GPU module string");
    return text;
}

static size_t strip_module_signature(unsigned char *data, size_t size)
{
    static const char magic[] = MODULE_SIGNATURE_MAGIC;
    struct module_signature signature;
    size_t trailer_size = sizeof(signature) + sizeof(magic) - 1;
    size_t signature_size;

    if (size < trailer_size ||
        memcmp(data + size - sizeof(magic) + 1, magic, sizeof(magic) - 1))
        return size;
    memcpy(&signature, data + size - trailer_size, sizeof(signature));
    signature_size = ntohl(signature.sig_len);
    if (signature_size >
        SIZE_MAX - signature.signer_len - signature.key_id_len - trailer_size)
        die("invalid GPU module signature");
    signature_size +=
        signature.signer_len + signature.key_id_len + trailer_size;
    if (signature_size > size)
        die("invalid GPU module signature");
    return size - signature_size;
}

static void patch_gpu_module(const char *path, long host_page_size)
{
    unsigned char *data;
    size_t size;
    size_t patched_size;
    Elf64_Ehdr *header;
    Elf64_Shdr *sections;
    Elf64_Sym *vram_create = NULL;
    size_t symtab_index = 0;
    size_t patch_offset = 0;
    uint32_t replacement;
    size_t i;
    int fd;

    if (host_page_size <= 4096 || host_page_size & 4095 ||
        host_page_size > (long)(0x7ffffU << 12))
        die("unsupported host page size for GPU compatibility: %ld",
            host_page_size);
    data = (unsigned char *)read_file(path, MAX_MODULE_SIZE, &size);
    patched_size = strip_module_signature(data, size);
    if (!range_ok(patched_size, 0, sizeof(*header)))
        die("truncated GPU module");
    header = (Elf64_Ehdr *)data;
    if (memcmp(header->e_ident, ELFMAG, SELFMAG) ||
        header->e_ident[EI_CLASS] != ELFCLASS64 ||
        header->e_ident[EI_DATA] != ELFDATA2LSB || header->e_type != ET_REL ||
        header->e_machine != EM_LOONGARCH ||
        header->e_shentsize != sizeof(*sections) || !header->e_shnum ||
        !range_ok(patched_size, header->e_shoff,
                  (size_t)header->e_shnum * sizeof(*sections)))
        die("unsupported GPU module ELF format");
    sections = (Elf64_Shdr *)(data + header->e_shoff);
    for (i = 0; i < header->e_shnum; i++) {
        Elf64_Shdr *symbols = &sections[i];
        Elf64_Shdr *strings;
        size_t j;

        if (symbols->sh_type != SHT_SYMTAB ||
            symbols->sh_entsize != sizeof(Elf64_Sym) ||
            symbols->sh_link >= header->e_shnum ||
            !range_ok(patched_size, symbols->sh_offset, symbols->sh_size))
            continue;
        strings = &sections[symbols->sh_link];
        for (j = 0; j < symbols->sh_size / sizeof(Elf64_Sym); j++) {
            Elf64_Sym *symbol = (Elf64_Sym *)(data + symbols->sh_offset) + j;

            if (!strcmp(
                    elf_string(data, patched_size, strings, symbol->st_name),
                    "virtio_gpu_vram_create")) {
                vram_create = symbol;
                symtab_index = i;
                break;
            }
        }
    }
    if (!vram_create || vram_create->st_shndx >= header->e_shnum ||
        !vram_create->st_size)
        die("cannot find GPU module VRAM allocator");
    for (i = 0; i < header->e_shnum; i++) {
        Elf64_Shdr *relocations = &sections[i];
        Elf64_Shdr *symbols;
        Elf64_Shdr *strings;
        size_t j;

        if (relocations->sh_type != SHT_RELA ||
            relocations->sh_info != vram_create->st_shndx ||
            relocations->sh_link != symtab_index ||
            relocations->sh_entsize != sizeof(Elf64_Rela) ||
            !range_ok(patched_size, relocations->sh_offset,
                      relocations->sh_size))
            continue;
        symbols = &sections[symtab_index];
        strings = &sections[symbols->sh_link];
        for (j = 0; j < relocations->sh_size / sizeof(Elf64_Rela); j++) {
            Elf64_Rela *relocation =
                (Elf64_Rela *)(data + relocations->sh_offset) + j;
            size_t symbol_index = ELF64_R_SYM(relocation->r_info);
            Elf64_Sym *symbol;
            Elf64_Shdr *code;

            if (symbol_index >= symbols->sh_size / sizeof(Elf64_Sym) ||
                relocation->r_offset < vram_create->st_value ||
                relocation->r_offset >=
                    vram_create->st_value + vram_create->st_size)
                continue;
            symbol = (Elf64_Sym *)(data + symbols->sh_offset) + symbol_index;
            if (strcmp(elf_string(data, patched_size, strings, symbol->st_name),
                       "drm_mm_insert_node_in_range"))
                continue;
            if (relocation->r_offset < 12)
                die("invalid GPU module allocator call");
            code = &sections[vram_create->st_shndx];
            patch_offset =
                (size_t)code->sh_offset + (size_t)relocation->r_offset - 12;
            if (!range_ok(patched_size, patch_offset, sizeof(uint32_t)) ||
                read_le32(data + patch_offset) != 0x00150007U)
                die("GPU module allocator layout changed");
            break;
        }
    }
    if (!patch_offset)
        die("cannot find GPU module allocator alignment argument");
    replacement = 0x14000000U | ((uint32_t)host_page_size >> 12) << 5 | 7U;
    write_le32(data + patch_offset, replacement);
    fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0)
        die("open %s: %s", path, strerror(errno));
    write_all(fd, data, patched_size);
    if (close(fd) < 0)
        die("close %s: %s", path, strerror(errno));
    free(data);
}

static void unpack_gpu_module(const char *source, const char *output)
{
    char *const args[] = {"zstd", "-q", "-d", "-c", "--", (char *)source, NULL};
    char *zstd;
    pid_t pid;
    int fd;
    int status;

    zstd = find_in_path("zstd");
    if (!zstd)
        die("zstd is required for mixed-page GPU compatibility");
    free(zstd);
    fd = open(output, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        die("create %s: %s", output, strerror(errno));
    pid = fork();
    if (pid < 0)
        die("fork zstd: %s", strerror(errno));
    if (pid == 0) {
        if (dup2(fd, STDOUT_FILENO) < 0)
            _exit(127);
        close(fd);
        execvp(args[0], args);
        _exit(127);
    }
    close(fd);
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            die("waitpid zstd: %s", strerror(errno));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status))
        die("cannot decompress installed GPU module");
}

bool gpu_compat_needed(const char *modules_dir)
{
    return sysconf(_SC_PAGE_SIZE) > 4096 && strstr(modules_dir, "-4k");
}

char *gpu_compat_prepare(const char *runtime, const char *modules_dir)
{
    char *source = find_module(modules_dir, "virtio-gpu");
    char *output = xasprintf("%s/virtio-gpu.ko", runtime);
    long page_size = sysconf(_SC_PAGE_SIZE);

    if (!strstr(source, ".ko.zst"))
        die("installed GPU module is not zstd-compressed: %s", source);
    unpack_gpu_module(source, output);
    patch_gpu_module(output, page_size);
    free(source);
    return output;
}

void feature_gpu_compat_host_prepare(struct host_ctx *ctx)
{
    if (ctx->opts->gpu_compat == FEATURE_ON ||
        gpu_compat_needed(ctx->modules_dir))
        ctx->compatible_gpu_module =
            gpu_compat_prepare(ctx->runtime, ctx->modules_dir);
}
