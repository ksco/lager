#ifndef LAGER_CONFIG_H
#define LAGER_CONFIG_H

#include "utils.h"

enum feature_policy {
    FEATURE_OFF,
    FEATURE_AUTO,
    FEATURE_ON,
};

struct lager_config {
    char *kernel;
    char *modules_dir;
    char *resolution;
    enum feature_policy gpu_compat;
    struct strvec env;
};

void load_lager_config(struct lager_config *cfg);
void free_lager_config(struct lager_config *cfg);
int handle_lager_config_cli(int argc, char **argv);

#endif
