#define _GNU_SOURCE
#include "utils.h"

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

void *xmalloc(size_t size)
{
    void *ptr = malloc(size ? size : 1);

    if (!ptr)
        die("out of memory");
    return ptr;
}

void *xrealloc(void *ptr, size_t size)
{
    void *new_ptr = realloc(ptr, size ? size : 1);

    if (!new_ptr)
        die("out of memory");
    return new_ptr;
}

char *xstrdup(const char *str)
{
    char *copy = strdup(str);

    if (!copy)
        die("out of memory");
    return copy;
}

char *xstrndup(const char *str, size_t len)
{
    char *copy = strndup(str, len);

    if (!copy)
        die("out of memory");
    return copy;
}

char *xasprintf(const char *fmt, ...)
{
    char *result;
    va_list ap;

    va_start(ap, fmt);
    if (vasprintf(&result, fmt, ap) < 0)
        die("out of memory");
    va_end(ap);
    return result;
}

void vec_push(struct strvec *vec, char *item)
{
    if (vec->len + 1 >= vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 16;
        vec->items = xrealloc(vec->items, vec->cap * sizeof(*vec->items));
    }
    vec->items[vec->len++] = item;
    vec->items[vec->len] = NULL;
}

void vec_push_copy(struct strvec *vec, const char *item)
{
    vec_push(vec, xstrdup(item));
}

static void buf_reserve(struct bytebuf *buf, size_t extra)
{
    size_t wanted = buf->len + extra;

    if (wanted <= buf->cap)
        return;
    buf->cap = buf->cap ? buf->cap : 256;
    while (buf->cap < wanted)
        buf->cap *= 2;
    buf->data = xrealloc(buf->data, buf->cap);
}

void buf_append(struct bytebuf *buf, const void *data, size_t len)
{
    buf_reserve(buf, len);
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
}

void buf_append_string(struct bytebuf *buf, const char *str)
{
    buf_append(buf, str, strlen(str) + 1);
}

void mkdir_ok(const char *path, mode_t mode)
{
    if (mkdir(path, mode) < 0 && errno != EEXIST)
        die("mkdir %s: %s", path, strerror(errno));
}

bool path_exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

bool executable(const char *path)
{
    return path && access(path, X_OK) == 0;
}

char *find_in_path(const char *name)
{
    char *path_copy;
    char *saveptr = NULL;
    char *part;

    if (strchr(name, '/'))
        return executable(name) ? xstrdup(name) : NULL;
    path_copy = xstrdup(getenv("PATH") ? getenv("PATH") : "");
    for (part = strtok_r(path_copy, ":", &saveptr); part;
         part = strtok_r(NULL, ":", &saveptr)) {
        char *candidate = xasprintf("%s/%s", part, name);

        if (executable(candidate)) {
            free(path_copy);
            return candidate;
        }
        free(candidate);
    }
    free(path_copy);
    return NULL;
}

char *find_program(const char *requested, const char *name,
                   const char *fallback)
{
    char *result;

    if (requested) {
        if (!executable(requested))
            die("%s is not executable", requested);
        return xstrdup(requested);
    }
    result = find_in_path(name);
    if (result)
        return result;
    if (fallback && executable(fallback))
        return xstrdup(fallback);
    return NULL;
}

static bool same_env_name(const char *entry, const char *assignment)
{
    const char *eq = strchr(assignment, '=');
    size_t len;

    if (!eq)
        die("environment value must use NAME=VALUE syntax: %s", assignment);
    len = (size_t)(eq - assignment);
    return strncmp(entry, assignment, len) == 0 && entry[len] == '=';
}

void env_set(struct strvec *env, const char *assignment)
{
    size_t i;

    if (!strchr(assignment, '='))
        die("environment value must use NAME=VALUE syntax: %s", assignment);
    for (i = 0; i < env->len; i++) {
        if (same_env_name(env->items[i], assignment)) {
            free(env->items[i]);
            env->items[i] = xstrdup(assignment);
            return;
        }
    }
    vec_push_copy(env, assignment);
}

void env_unset(struct strvec *env, const char *name)
{
    size_t len;
    size_t i = 0;

    if (!*name || strchr(name, '='))
        die("environment name must be non-empty and cannot contain '=': %s",
            name);
    len = strlen(name);
    while (i < env->len) {
        if (strncmp(env->items[i], name, len) == 0 &&
            env->items[i][len] == '=') {
            free(env->items[i]);
            memmove(&env->items[i], &env->items[i + 1],
                    (env->len - i) * sizeof(*env->items));
            env->len--;
            continue;
        }
        i++;
    }
}

const char *env_get(const struct strvec *env, const char *name)
{
    size_t len = strlen(name);
    size_t i;

    for (i = 0; i < env->len; i++) {
        if (strncmp(env->items[i], name, len) == 0 && env->items[i][len] == '=')
            return env->items[i] + len + 1;
    }
    return NULL;
}

struct strvec copy_host_env(void)
{
    struct strvec result = {0};
    size_t i;

    for (i = 0; environ[i]; i++)
        env_set(&result, environ[i]);
    return result;
}

char *read_file(const char *path, size_t max_size, size_t *size_out)
{
    struct stat st;
    char *data;
    int fd;
    size_t offset = 0;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        die("open %s: %s", path, strerror(errno));
    if (fstat(fd, &st) < 0)
        die("stat %s: %s", path, strerror(errno));
    if (st.st_size <= 0 || (size_t)st.st_size > max_size)
        die("invalid file size: %s", path);
    data = xmalloc((size_t)st.st_size);
    while (offset < (size_t)st.st_size) {
        ssize_t got = read(fd, data + offset, (size_t)st.st_size - offset);

        if (got < 0) {
            if (errno == EINTR)
                continue;
            die("read %s: %s", path, strerror(errno));
        }
        if (got == 0)
            die("short read from %s", path);
        offset += (size_t)got;
    }
    close(fd);
    *size_out = offset;
    return data;
}

void write_all(int fd, const void *data, size_t size)
{
    const unsigned char *cursor = data;

    while (size) {
        ssize_t written = write(fd, cursor, size);

        if (written < 0) {
            if (errno == EINTR)
                continue;
            die("write: %s", strerror(errno));
        }
        cursor += written;
        size -= (size_t)written;
    }
}
