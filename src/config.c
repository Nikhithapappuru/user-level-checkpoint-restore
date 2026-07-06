#define _GNU_SOURCE

#include "ulcr/config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *trim(char *value) {
    char *end = NULL;
    while (*value != '\0' && isspace((unsigned char) *value)) {
        ++value;
    }
    if (*value == '\0') {
        return value;
    }
    end = value + strlen(value) - 1;
    while (end > value && isspace((unsigned char) *end)) {
        *end = '\0';
        --end;
    }
    return value;
}

int ulcr_load_config(const char *config_path, ulcr_config_t *config) {
    FILE *fp = NULL;
    char line[1024];

    memset(config, 0, sizeof(*config));
    config->checkpoint_interval_sec = 15;
    config->monitor_interval_sec = 3;
    config->max_checkpoints = 5;
    config->auto_restore = 1;
    ulcr_copy_string(config->runtime_root, sizeof(config->runtime_root), "./runtime");

    fp = fopen(config_path, "r");
    if (fp == NULL) {
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *key = NULL;
        char *value = NULL;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || line[0] == '\n' || eq == NULL) {
            continue;
        }
        *eq = '\0';
        key = trim(line);
        value = trim(eq + 1);
        value[strcspn(value, "\r\n")] = '\0';

        if (strcmp(key, "process_name") == 0) {
            ulcr_copy_string(config->process_name, sizeof(config->process_name), value);
        } else if (strcmp(key, "workload_path") == 0) {
            ulcr_copy_string(config->workload_path, sizeof(config->workload_path), value);
        } else if (strcmp(key, "workload_args") == 0) {
            ulcr_copy_string(config->workload_args, sizeof(config->workload_args), value);
        } else if (strcmp(key, "runtime_root") == 0) {
            ulcr_copy_string(config->runtime_root, sizeof(config->runtime_root), value);
        } else if (strcmp(key, "checkpoint_interval_sec") == 0) {
            config->checkpoint_interval_sec = atoi(value);
        } else if (strcmp(key, "monitor_interval_sec") == 0) {
            config->monitor_interval_sec = atoi(value);
        } else if (strcmp(key, "max_checkpoints") == 0) {
            config->max_checkpoints = atoi(value);
        } else if (strcmp(key, "auto_restore") == 0) {
            config->auto_restore = atoi(value);
        }
    }
    fclose(fp);

    if (config->process_name[0] == '\0' || config->workload_path[0] == '\0') {
        return -1;
    }
    if (config->checkpoint_interval_sec < 1 || config->monitor_interval_sec < 1 || config->max_checkpoints < 1) {
        return -1;
    }
    return 0;
}

int ulcr_prepare_runtime_paths(const ulcr_config_t *config, ulcr_runtime_paths_t *paths) {
    char checkpoints_root[ULCR_MAX_PATH];
    char processes_root[ULCR_MAX_PATH];

    memset(paths, 0, sizeof(*paths));
    ulcr_copy_string(paths->runtime_dir, sizeof(paths->runtime_dir), config->runtime_root);
    ulcr_path_join(paths->state_dir, sizeof(paths->state_dir), config->runtime_root, "state");
    ulcr_path_join(paths->logs_dir, sizeof(paths->logs_dir), config->runtime_root, "logs");
    ulcr_path_join(checkpoints_root, sizeof(checkpoints_root), config->runtime_root, "checkpoints");
    ulcr_path_join(processes_root, sizeof(processes_root), config->runtime_root, "processes");
    ulcr_path_join(paths->process_dir, sizeof(paths->process_dir), processes_root, config->process_name);
    ulcr_path_join(paths->checkpoints_dir, sizeof(paths->checkpoints_dir), checkpoints_root, config->process_name);
    ulcr_path_join(paths->checkpoint_slot_a_dir, sizeof(paths->checkpoint_slot_a_dir), paths->checkpoints_dir, "slot_A");
    ulcr_path_join(paths->checkpoint_slot_b_dir, sizeof(paths->checkpoint_slot_b_dir), paths->checkpoints_dir, "slot_B");
    snprintf(paths->checkpoint_meta_file, sizeof(paths->checkpoint_meta_file), "%s/active.meta", paths->checkpoints_dir);
    snprintf(paths->checkpoint_lock_file, sizeof(paths->checkpoint_lock_file), "%s/checkpoint.lock", paths->checkpoints_dir);
    snprintf(paths->state_file, sizeof(paths->state_file), "%s/%s.state", paths->state_dir, config->process_name);
    snprintf(paths->pid_file, sizeof(paths->pid_file), "%s/pid", paths->process_dir);
    snprintf(paths->status_file, sizeof(paths->status_file), "%s/status.txt", paths->process_dir);
    snprintf(paths->process_log, sizeof(paths->process_log), "%s/process.log", paths->process_dir);
    snprintf(paths->monitor_log, sizeof(paths->monitor_log), "%s/%s-monitor.log", paths->logs_dir, config->process_name);
    return 0;
}
