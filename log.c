#define _GNU_SOURCE
#include "log.h"

#include "utils.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

void die(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "lager: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(EXIT_FAILURE);
}

void warnx(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "lager: warning: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

char *default_log_path(void)
{
    const char *home = getenv("HOME");

    if (!home || !*home)
        die("HOME is not set; cannot locate ~/.config/lager/log.txt");
    return xasprintf("%s/.config/lager/log.txt", home);
}

void prepare_log(const char *path)
{
    char *old_path = xasprintf("%s.old", path);
    int fd;

    if (rename(path, old_path) < 0 && errno != ENOENT)
        die("rename %s to %s: %s", path, old_path, strerror(errno));
    free(old_path);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        die("create %s: %s", path, strerror(errno));
    if (close(fd) < 0)
        die("close %s: %s", path, strerror(errno));
}

int open_log(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);

    if (fd >= 0 && fd <= STDERR_FILENO) {
        int safe_fd = fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);

        close(fd);
        return safe_fd;
    }
    return fd;
}

void silence_output_fd(int fd)
{
    if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0)
        _exit(127);
    if (fd > STDERR_FILENO)
        close(fd);
}

void silence_output(const char *path)
{
    int fd = open_log(path);

    if (fd < 0)
        _exit(127);
    silence_output_fd(fd);
}

static void set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL);

    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

pid_t spawn_stderr_tee(char *const argv[], int *stderr_fd)
{
    int err_pipe[2];
    pid_t pid;

    if (pipe2(err_pipe, O_CLOEXEC) < 0)
        die("pipe stderr: %s", strerror(errno));
    pid = fork();
    if (pid < 0)
        die("fork: %s", strerror(errno));
    if (pid == 0) {
        close(err_pipe[0]);
        if (dup2(err_pipe[1], STDERR_FILENO) < 0)
            _exit(127);
        close(err_pipe[1]);
        execvp(argv[0], argv);
        fprintf(stderr, "lager: exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(err_pipe[1]);
    set_nonblock(err_pipe[0]);
    *stderr_fd = err_pipe[0];
    return pid;
}

static void tee_write(int fd, const char *buf, size_t size)
{
    size_t offset = 0;

    while (offset < size) {
        ssize_t written = write(fd, buf + offset, size - offset);

        if (written < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = {.fd = fd, .events = POLLOUT};

                if (poll(&pfd, 1, -1) < 0 && errno != EINTR)
                    return;
                continue;
            }
            return;
        }
        if (written == 0)
            return;
        offset += (size_t)written;
    }
}

static void pump_stderr_tee(int *fd, bool *eof, int log_fd)
{
    char buf[4096];
    ssize_t got;

    got = read(*fd, buf, sizeof(buf));
    if (got < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        *eof = true;
        close(*fd);
        return;
    }
    if (got == 0) {
        *eof = true;
        close(*fd);
        return;
    }
    tee_write(STDERR_FILENO, buf, (size_t)got);
    tee_write(log_fd, buf, (size_t)got);
}

int wait_for_stderr_tee(pid_t pid, int stderr_fd, int log_fd, const char *name)
{
    bool child_exited = false;
    bool stderr_eof = false;
    int status = 0;

    while (!child_exited || !stderr_eof) {
        struct pollfd pfd = {
            .fd = stderr_fd,
            .events = POLLIN | POLLHUP | POLLERR,
        };
        int ready;

        if (!child_exited) {
            pid_t got = waitpid(pid, &status, WNOHANG);

            if (got == pid)
                child_exited = true;
            else if (got < 0 && errno != EINTR)
                die("waitpid %s: %s", name, strerror(errno));
        }
        if (stderr_eof) {
            while (!child_exited) {
                pid_t got = waitpid(pid, &status, 0);

                if (got == pid)
                    child_exited = true;
                else if (errno != EINTR)
                    die("waitpid %s: %s", name, strerror(errno));
            }
            break;
        }
        ready = poll(&pfd, 1, 50);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            die("poll %s stderr: %s", name, strerror(errno));
        }
        if (ready > 0 && pfd.revents)
            pump_stderr_tee(&stderr_fd, &stderr_eof, log_fd);
    }
    return status;
}
