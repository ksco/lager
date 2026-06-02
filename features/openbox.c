#define _GNU_SOURCE
#include "openbox.h"

#include "common.h"

#include "../guest_services.h"
#include "../lager.h"
#include "../log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

extern char **environ;

void feature_openbox_host_resolve(struct host_ctx *ctx)
{
    if (!ctx->x11)
        return;
    feature_require_executable("/usr/bin/openbox", "openbox", "x11 support", "openbox");
}

static void write_openbox_config(void)
{
    static const char context_start[] = "<context name=\"Root\">";
    static const char context_end[] = "</context>";
    char *config;
    char *start;
    char *end;
    size_t size;
    int fd;

    config = read_file("/etc/xdg/openbox/rc.xml", MAX_CONFIG_SIZE, &size);
    config = xrealloc(config, size + 1);
    config[size] = '\0';
    start = strstr(config, context_start);
    end = start ? strstr(start, context_end) : NULL;
    if (!start || !end)
        die("cannot find Openbox root menu bindings");
    end += sizeof(context_end) - 1;
    fd = open("/run/lager/openbox.xml", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        die("create /run/lager/openbox.xml: %s", strerror(errno));
    write_all(fd, config, (size_t)(start - config));
    write_all(fd, end, size - (size_t)(end - config));
    if (close(fd) < 0)
        die("close /run/lager/openbox.xml: %s", strerror(errno));
    free(config);
}

static void setup_guest_openbox(const struct guest_config *cfg, int log_fd)
{
    const char *ready_path = "/tmp/.lager-openbox-ready";
    int tries;

    unlink(ready_path);
    write_openbox_config();
    if (guest_service_fork(GUEST_SERVICE_OPENBOX) == 0) {
        if (setgid((gid_t)cfg->header.gid) < 0 || setuid((uid_t)cfg->header.uid) < 0)
            die("drop openbox privileges: %s", strerror(errno));
        environ = cfg->env;
        silence_output_fd(log_fd);
        execl("/usr/bin/openbox", "openbox", "--sm-disable", "--config-file", "/run/lager/openbox.xml", "--startup",
              "/usr/bin/touch /tmp/.lager-openbox-ready", (char *)NULL);
        die("exec openbox: %s", strerror(errno));
    }
    for (tries = 0; tries < 100; tries++) {
        if (path_exists(ready_path))
            return;
        if (guest_service_exited(GUEST_SERVICE_OPENBOX))
            die("openbox exited before initializing");
        usleep(50000);
    }
    die("openbox did not initialize");
}

void feature_openbox_guest_setup(struct guest_ctx *ctx)
{
    setup_guest_openbox(ctx->cfg, ctx->log_fd);
}

void feature_openbox_guest_stop(struct guest_ctx *ctx)
{
    (void)ctx;
    guest_service_stop(GUEST_SERVICE_OPENBOX);
}
