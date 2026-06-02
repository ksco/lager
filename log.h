#ifndef LAGER_LOG_H
#define LAGER_LOG_H

#include <sys/types.h>

void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
void warnx(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

char *default_log_path(void);
void prepare_log(const char *path);
int open_log(const char *path);
void silence_output_fd(int fd);
void silence_output(const char *path);

pid_t spawn_stderr_tee(char *const argv[], int *stderr_fd);
int wait_for_stderr_tee(pid_t pid, int stderr_fd, int log_fd, const char *name);

#endif
