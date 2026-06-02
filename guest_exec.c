#define _GNU_SOURCE
#include "guest_exec.h"

#include "log.h"
#include "utils.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static pid_t guest_command_pid = -1;

static void set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL);

    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static bool pump_fd(int from, int to, bool *eof)
{
    char buf[4096];
    ssize_t got;
    size_t offset = 0;

    got = read(from, buf, sizeof(buf));
    if (got < 0) {
        if (errno == EINTR)
            return true;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return false;
        *eof = true;
        return false;
    }
    if (got == 0) {
        *eof = true;
        return false;
    }
    while (offset < (size_t)got) {
        ssize_t written = write(to, buf + offset, (size_t)got - offset);

        if (written < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = {.fd = to, .events = POLLOUT};

                poll(&pfd, 1, -1);
                continue;
            }
            *eof = true;
            return false;
        }
        offset += (size_t)written;
    }
    return true;
}

static int wait_guest_command_pty(pid_t child, int master_fd)
{
    bool child_exited = false;
    bool master_eof = false;
    bool stdin_eof = false;
    int status = 0;

    set_nonblock(master_fd);
    set_nonblock(STDIN_FILENO);
    while (!child_exited || !master_eof) {
        struct pollfd pfds[2];
        nfds_t nfds = 0;
        int master_index = -1;
        int stdin_index = -1;
        int timeout = 50;
        int ready;

        if (!child_exited) {
            pid_t got = waitpid(child, &status, WNOHANG);

            if (got == child)
                child_exited = true;
            else if (got < 0 && errno != EINTR)
                die("waitpid guest command: %s", strerror(errno));
        }
        if (child_exited) {
            while (pump_fd(master_fd, STDOUT_FILENO, &master_eof))
                ;
            break;
        }
        if (!master_eof) {
            master_index = (int)nfds;
            pfds[nfds].fd = master_fd;
            pfds[nfds].events = POLLIN | POLLHUP | POLLERR;
            pfds[nfds].revents = 0;
            nfds++;
        }
        if (!stdin_eof && !child_exited) {
            stdin_index = (int)nfds;
            pfds[nfds].fd = STDIN_FILENO;
            pfds[nfds].events = POLLIN | POLLHUP | POLLERR;
            pfds[nfds].revents = 0;
            nfds++;
        }
        if (!nfds)
            break;
        ready = poll(pfds, nfds, timeout);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            die("poll guest command pty: %s", strerror(errno));
        }
        if (ready == 0) {
            if (child_exited)
                break;
            continue;
        }
        if (master_index >= 0 && pfds[master_index].revents)
            pump_fd(master_fd, STDOUT_FILENO, &master_eof);
        if (stdin_index >= 0 && pfds[stdin_index].revents)
            pump_fd(STDIN_FILENO, master_fd, &stdin_eof);
    }
    close(master_fd);
    if (!child_exited) {
        while (waitpid(child, &status, 0) < 0) {
            if (errno != EINTR)
                die("waitpid guest command: %s", strerror(errno));
        }
    }
    return status;
}

void guest_exec_signal(int signal_number)
{
    if (guest_command_pid > 0)
        kill(-guest_command_pid, signal_number);
}

int run_guest_command(const struct guest_config *cfg)
{
    int master_fd;
    char *slave_path;

    master_fd = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master_fd < 0 || grantpt(master_fd) < 0 || unlockpt(master_fd) < 0)
        die("create guest command pty: %s", strerror(errno));
    slave_path = ptsname(master_fd);
    if (!slave_path)
        die("resolve guest command pty: %s", strerror(errno));

    guest_command_pid = fork();
    if (guest_command_pid < 0)
        die("fork guest command: %s", strerror(errno));
    if (guest_command_pid == 0) {
        int slave_fd;

        if (setsid() < 0)
            die("create guest command session: %s", strerror(errno));
        slave_fd = open(slave_path, O_RDWR);
        if (slave_fd < 0)
            die("open guest command pty: %s", strerror(errno));
        if (ioctl(slave_fd, TIOCSCTTY, 0) < 0)
            die("set guest command controlling pty: %s", strerror(errno));
        if (dup2(slave_fd, STDIN_FILENO) < 0 || dup2(slave_fd, STDOUT_FILENO) < 0 || dup2(slave_fd, STDERR_FILENO) < 0)
            die("wire guest command pty: %s", strerror(errno));
        if (slave_fd > STDERR_FILENO)
            close(slave_fd);
        close(master_fd);
        if (setgid((gid_t)cfg->header.gid) < 0 || setuid((uid_t)cfg->header.uid) < 0)
            die("drop guest privileges: %s", strerror(errno));
        if (chdir(cfg->workdir) < 0) {
            warnx("cannot enter %s, using /", cfg->workdir);
            chdir("/");
        }
        environ = cfg->env;
        execvp(cfg->argv[0], cfg->argv);
        die("exec %s: %s", cfg->argv[0], strerror(errno));
    }
    return wait_guest_command_pty(guest_command_pid, master_fd);
}
