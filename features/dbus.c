#define _GNU_SOURCE
#include "dbus.h"

#include "common.h"

#include "../guest/services.h"
#include "../misc/log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern char **environ;

void feature_dbus_host_resolve(struct host_ctx *ctx)
{
    (void)ctx;
    feature_require_executable("/usr/bin/dbus-daemon", "dbus-daemon", "dbus support", "dbus");
    feature_require_executable("/usr/bin/dbus-send", "dbus-send", "dbus support", "dbus");
}

void feature_dbus_host_add_env(struct host_ctx *ctx)
{
    char *assignment = xasprintf("DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%lu/bus", (unsigned long)getuid());

    env_set(ctx->env, assignment);
    free(assignment);
}

static void write_guest_system_bus_config(const char *path)
{
    static const char config[] =
        "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\"\n"
        " \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
        "<busconfig>\n"
        "  <type>system</type>\n"
        "  <user>dbus</user>\n"
        "  <auth>EXTERNAL</auth>\n"
        "  <listen>unix:path=/run/dbus/system_bus_socket</listen>\n"
        "  <policy context=\"default\">\n"
        "    <allow user=\"*\"/>\n"
        "    <deny own=\"*\"/>\n"
        "    <deny send_type=\"method_call\"/>\n"
        "    <allow send_type=\"signal\"/>\n"
        "    <allow send_requested_reply=\"true\" send_type=\"method_return\"/>\n"
        "    <allow send_requested_reply=\"true\" send_type=\"error\"/>\n"
        "    <allow receive_type=\"method_call\"/>\n"
        "    <allow receive_type=\"method_return\"/>\n"
        "    <allow receive_type=\"error\"/>\n"
        "    <allow receive_type=\"signal\"/>\n"
        "    <allow send_destination=\"org.freedesktop.DBus\" send_interface=\"org.freedesktop.DBus\"/>\n"
        "    <allow send_destination=\"org.freedesktop.DBus\" send_interface=\"org.freedesktop.DBus.Introspectable\"/>\n"
        "    <allow send_destination=\"org.freedesktop.DBus\" send_interface=\"org.freedesktop.DBus.Properties\"/>\n"
        "    <allow send_destination=\"org.freedesktop.DBus\" send_interface=\"org.freedesktop.DBus.Containers1\"/>\n"
        "    <deny send_destination=\"org.freedesktop.DBus\" send_interface=\"org.freedesktop.DBus\"\n"
        "          send_member=\"UpdateActivationEnvironment\"/>\n"
        "    <deny send_destination=\"org.freedesktop.DBus\" send_interface=\"org.freedesktop.DBus.Debug.Stats\"/>\n"
        "    <deny send_destination=\"org.freedesktop.DBus\" send_interface=\"org.freedesktop.systemd1.Activator\"/>\n"
        "  </policy>\n"
        "  <policy user=\"root\">\n"
        "    <allow send_destination=\"org.freedesktop.DBus\" send_interface=\"org.freedesktop.systemd1.Activator\"/>\n"
        "    <allow send_destination=\"org.freedesktop.DBus\" send_interface=\"org.freedesktop.DBus.Monitoring\"/>\n"
        "    <allow send_destination=\"org.freedesktop.DBus\" send_interface=\"org.freedesktop.DBus.Debug.Stats\"/>\n"
        "  </policy>\n"
        "  <include ignore_missing=\"yes\">/etc/dbus-1/system.conf</include>\n"
        "  <includedir>/usr/share/dbus-1/system.d</includedir>\n"
        "  <includedir>/etc/dbus-1/system.d</includedir>\n"
        "  <include ignore_missing=\"yes\">/etc/dbus-1/system-local.conf</include>\n"
        "</busconfig>\n";
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

    if (fd < 0)
        die("create %s: %s", path, strerror(errno));
    write_all(fd, config, sizeof(config) - 1);
    if (close(fd) < 0)
        die("close %s: %s", path, strerror(errno));
}

static void setup_guest_dbus(const struct guest_config *cfg, int log_fd)
{
    const char *system_socket = "/run/dbus/system_bus_socket";
    const char *system_config = "/run/lager/system-bus.conf";
    char *runtime_dir = xasprintf("/run/user/%u", cfg->header.uid);
    char *session_socket = xasprintf("%s/bus", runtime_dir);
    char *session_address = xasprintf("--address=unix:path=%s", session_socket);

    mkdir_ok("/run/lager", 0755);
    mkdir_ok("/run/dbus", 0755);
    mkdir_ok("/run/user", 0755);
    mkdir_ok(runtime_dir, 0700);
    if (chmod(runtime_dir, 0700) < 0 || chown(runtime_dir, (uid_t)cfg->header.uid, (gid_t)cfg->header.gid) < 0)
        die("prepare %s: %s", runtime_dir, strerror(errno));
    write_guest_system_bus_config(system_config);
    unlink(system_socket);
    if (guest_service_fork(GUEST_SERVICE_SYSTEM_BUS) == 0) {
        silence_output_fd(log_fd);
        execl("/usr/bin/dbus-daemon", "dbus-daemon", "--nofork", "--nopidfile",
              "--config-file=/run/lager/system-bus.conf", (char *)NULL);
        die("exec system dbus-daemon: %s", strerror(errno));
    }
    guest_service_wait_path(GUEST_SERVICE_SYSTEM_BUS, system_socket);

    unlink(session_socket);
    if (guest_service_fork(GUEST_SERVICE_SESSION_BUS) == 0) {
        if (setgid((gid_t)cfg->header.gid) < 0 || setuid((uid_t)cfg->header.uid) < 0)
            die("drop session dbus-daemon privileges: %s", strerror(errno));
        environ = cfg->env;
        silence_output_fd(log_fd);
        execl("/usr/bin/dbus-daemon", "dbus-daemon", "--session", "--nofork", "--nopidfile", session_address,
              (char *)NULL);
        die("exec session dbus-daemon: %s", strerror(errno));
    }
    guest_service_wait_path(GUEST_SERVICE_SESSION_BUS, session_socket);
    free(session_address);
    free(session_socket);
    free(runtime_dir);
}

void feature_dbus_guest_setup(struct guest_ctx *ctx)
{
    setup_guest_dbus(ctx->cfg, ctx->log_fd);
}

void feature_dbus_guest_stop(struct guest_ctx *ctx)
{
    (void)ctx;
    guest_service_stop(GUEST_SERVICE_SESSION_BUS);
    guest_service_stop(GUEST_SERVICE_SYSTEM_BUS);
}
