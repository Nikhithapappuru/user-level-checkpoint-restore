#define _GNU_SOURCE

#include "ulcr/config.h"
#include "ulcr/manager.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static void usage(const char *argv0) {
    fprintf(stderr, "Usage: %s <init|start|checkpoint|monitor|restore|status|fail|stop> <config>\n", argv0);
}

int main(int argc, char **argv) {
    ulcr_config_t config;
    ulcr_runtime_paths_t paths;
    pid_t pid = 0;

    if (argc != 3) {
        usage(argv[0]);
        return 1;
    }
    if (ulcr_load_config(argv[2], &config) != 0) {
        fprintf(stderr, "failed to load config: %s\n", argv[2]);
        return 1;
    }
    ulcr_prepare_runtime_paths(&config, &paths);
    if (ulcr_init_runtime(&config, &paths) != 0) {
        fprintf(stderr, "failed to initialize runtime layout\n");
        return 1;
    }

    if (strcmp(argv[1], "init") == 0) {
        printf("runtime initialized at %s\n", paths.runtime_dir);
        return 0;
    }
    if (strcmp(argv[1], "start") == 0) {
        if (ulcr_start_process(&config, &paths, &pid) != 0) {
            fprintf(stderr, "failed to start managed process\n");
            return 1;
        }
        printf("started pid=%d\n", pid);
        return 0;
    }
    if (strcmp(argv[1], "checkpoint") == 0) {
        char buffer[64];
        if (ulcr_read_text_file(paths.pid_file, buffer, sizeof(buffer)) != 0) {
            fprintf(stderr, "missing pid file\n");
            return 1;
        }
        pid = (pid_t) atoi(buffer);
        if (ulcr_checkpoint_latest(&config, &paths, pid) != 0) {
            fprintf(stderr, "checkpoint failed\n");
            return 1;
        }
        printf("checkpoint created\n");
        return 0;
    }
    if (strcmp(argv[1], "monitor") == 0) {
        return ulcr_monitor_loop(&config, &paths) == 0 ? 0 : 1;
    }
    if (strcmp(argv[1], "restore") == 0) {
        if (ulcr_restore_latest(&config, &paths, &pid) != 0) {
            fprintf(stderr, "restore failed\n");
            return 1;
        }
        printf("restored pid=%d\n", pid);
        return 0;
    }
    if (strcmp(argv[1], "status") == 0) {
        return ulcr_print_status(&config, &paths);
    }
    if (strcmp(argv[1], "fail") == 0) {
        return ulcr_fail_process(&paths) == 0 ? 0 : 1;
    }
    if (strcmp(argv[1], "stop") == 0) {
        return ulcr_stop_process(&paths) == 0 ? 0 : 1;
    }

    usage(argv[0]);
    return 1;
}
