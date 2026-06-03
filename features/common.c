#define _GNU_SOURCE
#include "common.h"

#include "../misc/log.h"
#include "../misc/utils.h"

#include <errno.h>
#include <grp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

void feature_require_executable(const char *path, const char *program, const char *feature, const char *package)
{
    if (!executable(path))
        die("%s is required for %s; install the %s package", program, feature, package);
}

int feature_spawn_wait(char *const argv[], bool warn_failure)
{
    pid_t pid = fork();
    int status;

    if (pid < 0)
        die("fork: %s", strerror(errno));
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "lager: exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            die("waitpid: %s", strerror(errno));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (warn_failure)
            warnx("%s failed", argv[0]);
        return -1;
    }
    return 0;
}

int feature_spawn_wait_silent(char *const argv[], uid_t uid, gid_t gid)
{
    pid_t pid = fork();
    int status;

    if (pid < 0)
        die("fork: %s", strerror(errno));
    if (pid == 0) {
        if ((uid != 0 || gid != 0) && (setgroups(0, NULL) < 0 || setgid(gid) < 0 || setuid(uid) < 0))
            _exit(126);
        silence_output("/dev/null");
        execvp(argv[0], argv);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            die("waitpid: %s", strerror(errno));
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

bool feature_wait_for_guest_bus_name(const char *bus, const char *name, enum guest_service_id service_id,
                                     const char *process_name, uid_t uid, gid_t gid)
{
    char *check_owner[] = {
        "/usr/bin/dbus-send",
        (char *)bus,
        "--print-reply",
        "--dest=org.freedesktop.DBus",
        "/",
        "org.freedesktop.DBus.GetNameOwner",
        NULL,
        NULL,
    };
    int tries;

    check_owner[6] = xasprintf("string:%s", name);
    for (tries = 0; tries < 100; tries++) {
        if (guest_service_exited(service_id)) {
            warnx("%s exited before registering its D-Bus name", process_name);
            free(check_owner[6]);
            return false;
        }
        if (feature_spawn_wait_silent(check_owner, uid, gid) == 0) {
            free(check_owner[6]);
            return true;
        }
        usleep(50000);
    }
    warnx("%s did not register its D-Bus name", process_name);
    free(check_owner[6]);
    return false;
}

bool feature_guest_user_ids(const char *wanted, uid_t *uid, gid_t *gid)
{
    FILE *file = fopen("/etc/passwd", "r");
    char *line = NULL;
    size_t capacity = 0;
    bool found = false;

    if (!file)
        return false;
    while (getline(&line, &capacity, file) >= 0) {
        char *cursor = line;
        char *name = strsep(&cursor, ":");
        char *password = strsep(&cursor, ":");
        char *uid_text = strsep(&cursor, ":");
        char *gid_text = strsep(&cursor, ":");
        char *end;
        unsigned long value;

        if (!name || !password || !uid_text || !gid_text || strcmp(name, wanted))
            continue;
        errno = 0;
        value = strtoul(uid_text, &end, 10);
        if (errno || *end || value > UINT32_MAX)
            break;
        *uid = (uid_t)value;
        errno = 0;
        value = strtoul(gid_text, &end, 10);
        if (errno || *end || value > UINT32_MAX)
            break;
        *gid = (gid_t)value;
        found = true;
        break;
    }
    free(line);
    fclose(file);
    return found;
}
