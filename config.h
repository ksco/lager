#ifndef LAGER_CONFIG_H
#define LAGER_CONFIG_H

#include "utils.h"

enum feature_policy {
    FEATURE_OFF,
    FEATURE_AUTO,
    FEATURE_ON,
};

enum gpu_backend {
    GPU_BACKEND_AUTO,
    GPU_BACKEND_VIRGL,
    GPU_BACKEND_RUTABAGA,
};

struct lager_config {
    char *kernel;
    char *modules_dir;
    char *resolution;
    enum feature_policy gpu_compat;
    enum gpu_backend gpu_backend;
    struct strvec env;
};

void load_lager_config(struct lager_config *cfg);
void free_lager_config(struct lager_config *cfg);
int handle_lager_config_cli(int argc, char **argv);

#endif
