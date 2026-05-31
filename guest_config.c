#define _GNU_SOURCE
#include "guest_config.h"

#include "log.h"

#include <stdint.h>
#include <string.h>

static char *config_string(char **cursor, char *end)
{
    char *start = *cursor;
    char *nul;

    if (start >= end)
        die("truncated guest config");
    nul = memchr(start, '\0', (size_t)(end - start));
    if (!nul)
        die("invalid guest config string");
    *cursor = nul + 1;
    return start;
}

struct bytebuf make_guest_config(const struct config_header *header,
                                 const char *workdir, const char *box64,
                                 const char *log_path, char *const guest_argv[],
                                 const struct strvec *env)
{
    struct bytebuf buf = {0};
    uint32_t i;

    buf_append(&buf, header, sizeof(*header));
    buf_append_string(&buf, workdir);
    buf_append_string(&buf, box64 ? box64 : "");
    buf_append_string(&buf, log_path);
    for (i = 0; i < header->argc; i++)
        buf_append_string(&buf, guest_argv[i]);
    for (i = 0; i < header->envc; i++)
        buf_append_string(&buf, env->items[i]);
    return buf;
}

struct guest_config read_guest_config(void)
{
    struct guest_config cfg = {0};
    char *cursor;
    char *end;
    size_t size;
    uint32_t i;

    cfg.storage = read_file("/lager/config", MAX_CONFIG_SIZE, &size);
    if (size < sizeof(cfg.header))
        die("truncated guest config header");
    memcpy(&cfg.header, cfg.storage, sizeof(cfg.header));
    if (memcmp(cfg.header.magic, CFG_MAGIC, sizeof(cfg.header.magic)) ||
        cfg.header.version != CFG_VERSION)
        die("unsupported guest config");
    cursor = (char *)cfg.storage + sizeof(cfg.header);
    end = (char *)cfg.storage + size;
    if (cfg.header.argc == 0 || cfg.header.argc > 4096 ||
        cfg.header.envc > 16384)
        die("invalid guest config counts");
    cfg.workdir = config_string(&cursor, end);
    cfg.box64 = config_string(&cursor, end);
    cfg.log_path = config_string(&cursor, end);
    cfg.argv = xmalloc((cfg.header.argc + 1) * sizeof(*cfg.argv));
    for (i = 0; i < cfg.header.argc; i++)
        cfg.argv[i] = config_string(&cursor, end);
    cfg.argv[cfg.header.argc] = NULL;
    cfg.env = xmalloc((cfg.header.envc + 1) * sizeof(*cfg.env));
    for (i = 0; i < cfg.header.envc; i++)
        cfg.env[i] = config_string(&cursor, end);
    cfg.env[cfg.header.envc] = NULL;
    return cfg;
}
