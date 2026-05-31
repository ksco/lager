#ifndef LAGER_UTILS_H
#define LAGER_UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>

struct strvec {
    char **items;
    size_t len;
    size_t cap;
};

struct bytebuf {
    unsigned char *data;
    size_t len;
    size_t cap;
};

void *xmalloc(size_t size);
void *xrealloc(void *ptr, size_t size);
char *xstrdup(const char *str);
char *xstrndup(const char *str, size_t len);
char *xasprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void vec_push(struct strvec *vec, char *item);
void vec_push_copy(struct strvec *vec, const char *item);
void buf_append(struct bytebuf *buf, const void *data, size_t len);
void buf_append_string(struct bytebuf *buf, const char *str);

void mkdir_ok(const char *path, mode_t mode);
bool path_exists(const char *path);
bool executable(const char *path);
char *find_in_path(const char *name);
char *find_program(const char *requested, const char *name,
                   const char *fallback);

void env_set(struct strvec *env, const char *assignment);
const char *env_get(const struct strvec *env, const char *name);
struct strvec copy_host_env(void);

char *read_file(const char *path, size_t max_size, size_t *size_out);
void write_all(int fd, const void *data, size_t size);

#endif
