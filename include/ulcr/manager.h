#ifndef ULCR_MANAGER_H
#define ULCR_MANAGER_H

#include "config.h"

int ulcr_init_runtime(const ulcr_config_t *config, const ulcr_runtime_paths_t *paths);
int ulcr_start_process(const ulcr_config_t *config, const ulcr_runtime_paths_t *paths, pid_t *pid_out);
int ulcr_checkpoint_latest(const ulcr_config_t *config, const ulcr_runtime_paths_t *paths, pid_t pid);
int ulcr_restore_latest(const ulcr_config_t *config, const ulcr_runtime_paths_t *paths, pid_t *restored_pid);
int ulcr_monitor_loop(const ulcr_config_t *config, const ulcr_runtime_paths_t *paths);
int ulcr_fail_process(const ulcr_runtime_paths_t *paths);
int ulcr_stop_process(const ulcr_runtime_paths_t *paths);
int ulcr_print_status(const ulcr_config_t *config, const ulcr_runtime_paths_t *paths);

#endif
