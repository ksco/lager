#ifndef LAGER_GUEST_CONFIG_H
#define LAGER_GUEST_CONFIG_H

#include "../misc/config.h"
#include "../misc/utils.h"

struct guest_config {
    struct config_header header;
    char *workdir;
    char *box64;
    char *log_path;
    char **argv;
    char **env;
    void *storage;
};

struct bytebuf make_guest_config(const struct config_header *header, const char *workdir, const char *box64,
                                 const char *log_path, char *const guest_argv[], const struct strvec *env);
struct guest_config read_guest_config(void);

#endif
