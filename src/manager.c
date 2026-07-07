#define _GNU_SOURCE

#include "ulcr/manager.h"
#include "ulcr/procfs.h"
#include "ulcr/snapshot.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ULCR_SLOT_A "A"
#define ULCR_SLOT_B "B"

static int write_status_file(const ulcr_runtime_paths_t *paths, const char *value) {
    return ulcr_atomic_write_text_file(paths->status_file, value);
}

static int read_pid_from_file(const char *path, pid_t *pid) {
    char buffer[64];
    if (ulcr_read_text_file(path, buffer, sizeof(buffer)) != 0) {
        return -1;
    }
    *pid = (pid_t) atoi(buffer);
    return *pid > 0 ? 0 : -1;
}

static int write_state_line(const ulcr_runtime_paths_t *paths, const char *key, const char *value) {
    char line[1024];
    snprintf(line, sizeof(line), "%s=%s\n", key, value);
    return ulcr_atomic_write_text_file(paths->state_file, line);
}

static int checkpoint_lock_try_acquire(const ulcr_runtime_paths_t *paths, int *lock_fd) {
    *lock_fd = open(paths->checkpoint_lock_file, O_WRONLY | O_CREAT, 0644);
    if (*lock_fd < 0) {
        return -1;
    }
    if (flock(*lock_fd, LOCK_EX | LOCK_NB) != 0) {
        close(*lock_fd);
        *lock_fd = -1;
        return -1;
    }
    return 0;
}

static void checkpoint_lock_release(int lock_fd) {
    if (lock_fd >= 0) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
    }
}

static int read_active_slot(const ulcr_runtime_paths_t *paths, char *slot_buffer, size_t slot_buffer_size) {
    char buffer[256];
    char *line = NULL;

    if (!ulcr_file_exists(paths->checkpoint_meta_file)) {
        return -1;
    }
    if (ulcr_read_text_file(paths->checkpoint_meta_file, buffer, sizeof(buffer)) != 0) {
        return -1;
    }
    line = strstr(buffer, "active_slot=");
    if (line == NULL) {
        return -1;
    }
    line += strlen("active_slot=");
    line[strcspn(line, "\r\n")] = '\0';
    return ulcr_copy_string(slot_buffer, slot_buffer_size, line);
}

static int write_active_metadata(
    const ulcr_runtime_paths_t *paths,
    const char *slot_name,
    int interval_seconds,
    double runtime_seconds
) {
    char buffer[512];
    char timestamp[64];

    if (ulcr_timestamp_now(timestamp, sizeof(timestamp)) != 0) {
        return -1;
    }
    snprintf(
        buffer,
        sizeof(buffer),
        "active_slot=%s\nupdated_at=%s\ncheckpoint_interval_sec=%d\nruntime_seconds=%.2f\n",
        slot_name,
        timestamp,
        interval_seconds,
        runtime_seconds
    );
    return ulcr_atomic_write_text_file(paths->checkpoint_meta_file, buffer);
}

static int slot_directory_for_name(
    const ulcr_runtime_paths_t *paths,
    const char *slot_name,
    char *buffer,
    size_t buffer_size
) {
    if (strcmp(slot_name, ULCR_SLOT_A) == 0) {
        return ulcr_copy_string(buffer, buffer_size, paths->checkpoint_slot_a_dir);
    }
    if (strcmp(slot_name, ULCR_SLOT_B) == 0) {
        return ulcr_copy_string(buffer, buffer_size, paths->checkpoint_slot_b_dir);
    }
    return -1;
}

static int choose_next_slot(const ulcr_runtime_paths_t *paths, char *slot_buffer, size_t slot_buffer_size) {
    char active_slot[8];

    if (read_active_slot(paths, active_slot, sizeof(active_slot)) != 0) {
        return ulcr_copy_string(slot_buffer, slot_buffer_size, ULCR_SLOT_A);
    }
    if (strcmp(active_slot, ULCR_SLOT_A) == 0) {
        return ulcr_copy_string(slot_buffer, slot_buffer_size, ULCR_SLOT_B);
    }
    return ulcr_copy_string(slot_buffer, slot_buffer_size, ULCR_SLOT_A);
}

static int count_available_slots(const ulcr_runtime_paths_t *paths) {
    char path[ULCR_MAX_PATH];
    int count = 0;

    snprintf(path, sizeof(path), "%s/snapshot.bin", paths->checkpoint_slot_a_dir);
    if (ulcr_file_exists(path)) {
        ++count;
    }
    snprintf(path, sizeof(path), "%s/snapshot.bin", paths->checkpoint_slot_b_dir);
    if (ulcr_file_exists(path)) {
        ++count;
    }
    return count;
}

static int compute_dynamic_checkpoint_interval(const ulcr_config_t *config, pid_t pid) {
    double runtime_seconds = 0.0;
    int base_interval = config->checkpoint_interval_sec;

    if (ulcr_get_process_runtime_seconds(pid, &runtime_seconds) != 0) {
        return base_interval;
    }
    if (runtime_seconds >= 300.0) {
        return base_interval > 3 ? (base_interval / 3 > 0 ? base_interval / 3 : 1) : base_interval;
    }
    if (runtime_seconds >= 60.0) {
        return base_interval > 2 ? base_interval / 2 : 1;
    }
    return base_interval;
}

static int build_workload_argv(const ulcr_config_t *config, char **argv, size_t max_args) {
    static char args_copy[256];
    size_t argc = 0;
    char *token = NULL;

    if (max_args < 2) {
        return -1;
    }
    argv[argc++] = (char *) config->workload_path;
    args_copy[0] = '\0';
    ulcr_copy_string(args_copy, sizeof(args_copy), config->workload_args);
    token = strtok(args_copy, " ");
    while (token != NULL && argc + 1 < max_args) {
        argv[argc++] = token;
        token = strtok(NULL, " ");
    }
    argv[argc] = NULL;
    return (int) argc;
}

static int active_checkpoint_dir(const ulcr_runtime_paths_t *paths, char *buffer, size_t buffer_size, char *slot_name, size_t slot_name_size) {
    char active_slot[8];

    if (read_active_slot(paths, active_slot, sizeof(active_slot)) == 0) {
        if (slot_name != NULL) {
            ulcr_copy_string(slot_name, slot_name_size, active_slot);
        }
        return slot_directory_for_name(paths, active_slot, buffer, buffer_size);
    }

    if (ulcr_file_exists(paths->checkpoint_slot_a_dir)) {
        char snapshot_path[ULCR_MAX_PATH];
        snprintf(snapshot_path, sizeof(snapshot_path), "%s/snapshot.bin", paths->checkpoint_slot_a_dir);
        if (ulcr_file_exists(snapshot_path)) {
            if (slot_name != NULL) {
                ulcr_copy_string(slot_name, slot_name_size, ULCR_SLOT_A);
            }
            return ulcr_copy_string(buffer, buffer_size, paths->checkpoint_slot_a_dir);
        }
    }

    {
        char snapshot_path[ULCR_MAX_PATH];
        snprintf(snapshot_path, sizeof(snapshot_path), "%s/snapshot.bin", paths->checkpoint_slot_b_dir);
        if (ulcr_file_exists(snapshot_path)) {
            if (slot_name != NULL) {
                ulcr_copy_string(slot_name, slot_name_size, ULCR_SLOT_B);
            }
            return ulcr_copy_string(buffer, buffer_size, paths->checkpoint_slot_b_dir);
        }
    }

    return -1;
}

int ulcr_init_runtime(const ulcr_config_t *config, const ulcr_runtime_paths_t *paths) {
    (void) config;
    return ulcr_ensure_directory(paths->runtime_dir) == 0 &&
           ulcr_ensure_directory(paths->state_dir) == 0 &&
           ulcr_ensure_directory(paths->logs_dir) == 0 &&
           ulcr_ensure_directory(paths->process_dir) == 0 &&
           ulcr_ensure_directory(paths->checkpoints_dir) == 0 &&
           ulcr_ensure_directory(paths->checkpoint_slot_a_dir) == 0 &&
           ulcr_ensure_directory(paths->checkpoint_slot_b_dir) == 0 ? 0 : -1;
}

int ulcr_start_process(const ulcr_config_t *config, const ulcr_runtime_paths_t *paths, pid_t *pid_out) {
    char *argv[32];
    pid_t child = 0;
    int log_fd = -1;

    if (ulcr_file_exists(paths->pid_file)) {
        pid_t existing = 0;
        if (read_pid_from_file(paths->pid_file, &existing) == 0 && ulcr_process_exists(existing)) {
            return -1;
        }
    }

    if (build_workload_argv(config, argv, 32) < 0) {
        return -1;
    }
    log_fd = open(paths->process_log, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0) {
        return -1;
    }

    child = fork();
    if (child < 0) {
        close(log_fd);
        return -1;
    }
    if (child == 0) {
        setsid();
        dup2(log_fd, STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);
        close(log_fd);
        execv(config->workload_path, argv);
        _exit(127);
    }
    close(log_fd);

    {
        char pid_buffer[32];
        snprintf(pid_buffer, sizeof(pid_buffer), "%d\n", child);
        ulcr_write_text_file(paths->pid_file, pid_buffer);
        write_state_line(paths, "status", "running");
        write_status_file(paths, "running\n");
    }
    *pid_out = child;
    return 0;
}

int ulcr_checkpoint_latest(const ulcr_config_t *config, const ulcr_runtime_paths_t *paths, pid_t pid) {
    char snapshot_dir[ULCR_MAX_PATH];
    char slot_name[8];
    char interval_buffer[64];
    int lock_fd = -1;
    int interval_seconds = 0;
    double runtime_seconds = 0.0;

    if (!ulcr_process_exists(pid)) {
        return -1;
    }
    if (checkpoint_lock_try_acquire(paths, &lock_fd) != 0) {
        return -1;
    }

    if (choose_next_slot(paths, slot_name, sizeof(slot_name)) != 0) {
        checkpoint_lock_release(lock_fd);
        return -1;
    }
    if (slot_directory_for_name(paths, slot_name, snapshot_dir, sizeof(snapshot_dir)) != 0) {
        checkpoint_lock_release(lock_fd);
        return -1;
    }
    if (ulcr_capture_snapshot(pid, config->process_name, snapshot_dir) != 0) {
        checkpoint_lock_release(lock_fd);
        return -1;
    }
    interval_seconds = compute_dynamic_checkpoint_interval(config, pid);
    if (ulcr_get_process_runtime_seconds(pid, &runtime_seconds) != 0) {
        runtime_seconds = 0.0;
    }
    if (write_active_metadata(paths, slot_name, interval_seconds, runtime_seconds) != 0) {
        checkpoint_lock_release(lock_fd);
        return -1;
    }
    snprintf(interval_buffer, sizeof(interval_buffer), "%d", interval_seconds);
    write_state_line(paths, "checkpoint_slot", slot_name);
    write_status_file(paths, "checkpointed\n");
    write_state_line(paths, "checkpoint_interval_sec", interval_buffer);
    checkpoint_lock_release(lock_fd);
    return 0;
}

int ulcr_restore_latest(const ulcr_config_t *config, const ulcr_runtime_paths_t *paths, pid_t *restored_pid) {
    char snapshot_dir[ULCR_MAX_PATH];
    char slot_name[8];
    int pid = -1;

    if (active_checkpoint_dir(paths, snapshot_dir, sizeof(snapshot_dir), slot_name, sizeof(slot_name)) != 0) {
        return -1;
    }
    pid = ulcr_restore_managed_process(
        snapshot_dir,
        paths->process_log,
        config->workload_path,
        config->workload_args
    );
    if (pid < 0) {
        return -1;
    }
    {
        char pid_buffer[32];
        snprintf(pid_buffer, sizeof(pid_buffer), "%d\n", pid);
        ulcr_write_text_file(paths->pid_file, pid_buffer);
        write_state_line(paths, "status", "restored");
        write_state_line(paths, "checkpoint_slot", slot_name);
        write_status_file(paths, "restored\n");
    }
    *restored_pid = (pid_t) pid;
    return 0;
}

int ulcr_monitor_loop(const ulcr_config_t *config, const ulcr_runtime_paths_t *paths) {
    int timer_fd = -1;
    struct itimerspec spec;
    time_t last_checkpoint = 0;
    pid_t pid = 0;
    int current_interval = config->monitor_interval_sec;

    memset(&spec, 0, sizeof(spec));
    spec.it_value.tv_sec = config->monitor_interval_sec;
    spec.it_interval.tv_sec = config->monitor_interval_sec;

    if (read_pid_from_file(paths->pid_file, &pid) != 0) {
        return -1;
    }
    timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (timer_fd < 0) {
        return -1;
    }
    if (timerfd_settime(timer_fd, 0, &spec, NULL) != 0) {
        close(timer_fd);
        return -1;
    }

    for (;;) {
        uint64_t expirations = 0;
        if (read(timer_fd, &expirations, sizeof(expirations)) != (ssize_t) sizeof(expirations)) {
            close(timer_fd);
            return -1;
        }

        if (!ulcr_process_exists(pid)) {
            if (config->auto_restore) {
                if (ulcr_restore_latest(config, paths, &pid) == 0) {
                    last_checkpoint = time(NULL);
                    current_interval = compute_dynamic_checkpoint_interval(config, pid);
                }
            }
            continue;
        }

        current_interval = compute_dynamic_checkpoint_interval(config, pid);
        if ((time(NULL) - last_checkpoint) >= current_interval) {
            if (ulcr_checkpoint_latest(config, paths, pid) == 0) {
                last_checkpoint = time(NULL);
            }
        }
    }
}

int ulcr_fail_process(const ulcr_runtime_paths_t *paths) {
    pid_t pid = 0;
    if (read_pid_from_file(paths->pid_file, &pid) != 0) {
        return -1;
    }
    if (kill(pid, SIGKILL) != 0) {
        return -1;
    }
    write_state_line(paths, "status", "failed");
    write_status_file(paths, "failed\n");
    return 0;
}

int ulcr_stop_process(const ulcr_runtime_paths_t *paths) {
    pid_t pid = 0;
    if (read_pid_from_file(paths->pid_file, &pid) != 0) {
        return -1;
    }
    if (kill(pid, SIGTERM) != 0) {
        return -1;
    }
    write_state_line(paths, "status", "stopped");
    write_status_file(paths, "stopped\n");
    return 0;
}

int ulcr_print_status(const ulcr_config_t *config, const ulcr_runtime_paths_t *paths) {
    char latest[ULCR_MAX_PATH] = "<none>";
    char slot_name[8] = "<none>";
    pid_t pid = 0;
    int alive = 0;
    int current_interval = 0;
    double runtime_seconds = 0.0;

    if (read_pid_from_file(paths->pid_file, &pid) == 0) {
        alive = ulcr_process_exists(pid);
        current_interval = compute_dynamic_checkpoint_interval(config, pid);
        if (ulcr_get_process_runtime_seconds(pid, &runtime_seconds) != 0) {
            runtime_seconds = 0.0;
        }
    }
    if (active_checkpoint_dir(paths, latest, sizeof(latest), slot_name, sizeof(slot_name)) != 0) {
        ulcr_copy_string(latest, sizeof(latest), "<none>");
        ulcr_copy_string(slot_name, sizeof(slot_name), "<none>");
    }

    printf("process_name: %s\n", config->process_name);
    printf("pid: %d\n", pid);
    printf("alive: %s\n", alive ? "yes" : "no");
    printf("checkpoint_slots_available: %d\n", count_available_slots(paths));
    printf("active_checkpoint_slot: %s\n", slot_name);
    printf("active_checkpoint_dir: %s\n", latest);
    printf("dynamic_checkpoint_interval_sec: %d\n", current_interval);
    printf("runtime_seconds: %.2f\n", runtime_seconds);
    printf("checkpoint_meta_file: %s\n", paths->checkpoint_meta_file);
    printf("process_log: %s\n", paths->process_log);
    printf("state_file: %s\n", paths->state_file);
    return 0;
}
