#define _GNU_SOURCE
#include "modules.h"

#include "log.h"
#include "utils.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char *find_module_recursive(const char *dir, const char *base)
{
    DIR *stream = opendir(dir);
    struct dirent *entry;

    if (!stream)
        return NULL;
    while ((entry = readdir(stream))) {
        char *path;
        struct stat st;

        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        path = xasprintf("%s/%s", dir, entry->d_name);
        if (lstat(path, &st) < 0) {
            free(path);
            continue;
        }
        if (S_ISREG(st.st_mode) && !strcmp(entry->d_name, base)) {
            closedir(stream);
            return path;
        }
        if (S_ISDIR(st.st_mode)) {
            char *found = find_module_recursive(path, base);

            free(path);
            if (found) {
                closedir(stream);
                return found;
            }
        } else {
            free(path);
        }
    }
    closedir(stream);
    return NULL;
}

char *find_module(const char *modules_dir, const char *name)
{
    static const char *suffixes[] = {".ko.zst", ".ko.xz", ".ko.gz", ".ko"};
    size_t i;

    for (i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        char *base = xasprintf("%s%s", name, suffixes[i]);
        char *found = find_module_recursive(modules_dir, base);

        free(base);
        if (found)
            return found;
    }
    die("cannot find %s kernel module below %s", name, modules_dir);
}
