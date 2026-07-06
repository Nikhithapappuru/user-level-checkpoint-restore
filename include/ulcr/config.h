#ifndef ULCR_CONFIG_H
#define ULCR_CONFIG_H

#include "common.h"

typedef struct ulcr_config {
    char process_name[64];
    char workload_path[ULCR_MAX_PATH];
    char workload_args[256];
    char runtime_root[ULCR_MAX_PATH];
    int checkpoint_interval_sec;
    int monitor_interval_sec;
    int max_checkpoints;
    int auto_restore;
} ulcr_config_t;

int ulcr_load_config(const char *config_path, ulcr_config_t *config);
int ulcr_prepare_runtime_paths(const ulcr_config_t *config, ulcr_runtime_paths_t *paths);

#endif
