#define _GNU_SOURCE
#include "audio.h"

#include "common.h"

#include "../guest_services.h"
#include "../log.h"

#include <errno.h>
#include <glob.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern char **environ;

static char *host_audio_backend(void)
{
    const char *runtime = getenv("PIPEWIRE_RUNTIME_DIR");
    char *path;

    if (!runtime || !*runtime)
        runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && *runtime) {
        path = xasprintf("%s/pipewire-0", runtime);
        if (path_exists(path)) {
            free(path);
            return xstrdup("pipewire");
        }
        free(path);

        path = xasprintf("%s/pulse/native", runtime);
        if (path_exists(path)) {
            free(path);
            return xstrdup("pa");
        }
        free(path);
    }
    if (getenv("PULSE_SERVER"))
        return xstrdup("pa");
    return NULL;
}

void feature_audio_host_resolve(struct host_ctx *ctx)
{
    feature_require_executable("/usr/bin/pulseaudio", "pulseaudio",
                               "audio support", "pulseaudio");
    ctx->audio_backend = host_audio_backend();
    if (!ctx->audio_backend)
        die("no PipeWire or PulseAudio server found; audio requires a running "
            "host audio server");
}

void feature_audio_host_add_env(struct host_ctx *ctx)
{
    char *assignment = xasprintf("PULSE_SERVER=unix:/run/user/%lu/pulse/native",
                                 (unsigned long)getuid());

    env_set(ctx->env, assignment);
    free(assignment);
}

void feature_audio_host_add_qemu_options(struct host_ctx *ctx)
{
    char *audiodev = xasprintf("%s,id=audio0", ctx->audio_backend);

    vec_push_copy(ctx->qemu, "-audiodev");
    vec_push(ctx->qemu, audiodev);
    vec_push_copy(ctx->qemu, "-device");
    vec_push_copy(ctx->qemu, "virtio-sound-pci,audiodev=audio0");
}

static void setup_guest_audio(const struct guest_config *cfg, int log_fd)
{
    glob_t nodes;
    char *pulse_socket;
    bool found_node = false;
    int tries;

    for (tries = 0; tries < 50; tries++) {
        memset(&nodes, 0, sizeof(nodes));
        if (glob("/dev/snd/*", 0, NULL, &nodes) == 0)
            break;
        globfree(&nodes);
        usleep(20000);
    }
    if (tries == 50) {
        die("guest audio device did not appear");
    }
    for (size_t i = 0; i < nodes.gl_pathc; i++) {
        struct stat st;

        if (lstat(nodes.gl_pathv[i], &st) < 0) {
            warnx("cannot inspect %s: %s", nodes.gl_pathv[i], strerror(errno));
            continue;
        }
        if (!S_ISCHR(st.st_mode))
            continue;
        found_node = true;
        if (chown(nodes.gl_pathv[i], (uid_t)cfg->header.uid,
                  (gid_t)cfg->header.gid) < 0)
            warnx("cannot chown %s: %s", nodes.gl_pathv[i], strerror(errno));
        if (chmod(nodes.gl_pathv[i], 0600) < 0)
            warnx("cannot chmod %s: %s", nodes.gl_pathv[i], strerror(errno));
    }
    globfree(&nodes);
    if (!found_node)
        die("guest audio device did not expose any character devices");
    pulse_socket = xasprintf("/run/user/%u/pulse/native", cfg->header.uid);
    if (guest_service_fork(GUEST_SERVICE_AUDIO) == 0) {
        if (setgid((gid_t)cfg->header.gid) < 0 ||
            setuid((uid_t)cfg->header.uid) < 0)
            die("drop pulseaudio privileges: %s", strerror(errno));
        environ = cfg->env;
        silence_output_fd(log_fd);
        execl("/usr/bin/pulseaudio", "pulseaudio", "--daemonize=no",
              "--exit-idle-time=-1", "--log-target=stderr", "-n", "-L",
              "module-native-protocol-unix", "-L",
              "module-alsa-sink device=hw:0,0 sink_name=virtio_snd", "-L",
              "module-always-sink", (char *)NULL);
        die("exec pulseaudio: %s", strerror(errno));
    }
    for (tries = 0; tries < 100; tries++) {
        if (path_exists(pulse_socket)) {
            free(pulse_socket);
            return;
        }
        if (guest_service_exited(GUEST_SERVICE_AUDIO))
            die("pulseaudio exited before creating %s", pulse_socket);
        usleep(50000);
    }
    die("pulseaudio did not create %s", pulse_socket);
}

void feature_audio_guest_setup(struct guest_ctx *ctx)
{
    setup_guest_audio(ctx->cfg, ctx->log_fd);
}

void feature_audio_guest_stop(struct guest_ctx *ctx)
{
    (void)ctx;
    guest_service_stop(GUEST_SERVICE_AUDIO);
}
