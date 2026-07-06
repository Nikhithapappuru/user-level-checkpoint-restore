#ifndef ULCR_COMMON_H
#define ULCR_COMMON_H

#define _GNU_SOURCE

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define ULCR_MAX_PATH 512
#define ULCR_MAX_REGIONS 512
#define ULCR_MAX_FDS 128
#define ULCR_REGION_NAME 256
#define ULCR_MAGIC 0x554C4352u
#define ULCR_VERSION 1u

typedef struct ulcr_runtime_paths {
    char runtime_dir[ULCR_MAX_PATH];
    char state_dir[ULCR_MAX_PATH];
    char logs_dir[ULCR_MAX_PATH];
    char process_dir[ULCR_MAX_PATH];
    char checkpoints_dir[ULCR_MAX_PATH];
    char checkpoint_slot_a_dir[ULCR_MAX_PATH];
    char checkpoint_slot_b_dir[ULCR_MAX_PATH];
    char checkpoint_meta_file[ULCR_MAX_PATH];
    char checkpoint_lock_file[ULCR_MAX_PATH];
    char state_file[ULCR_MAX_PATH];
    char pid_file[ULCR_MAX_PATH];
    char status_file[ULCR_MAX_PATH];
    char process_log[ULCR_MAX_PATH];
    char monitor_log[ULCR_MAX_PATH];
} ulcr_runtime_paths_t;

int ulcr_ensure_directory(const char *path);
int ulcr_write_text_file(const char *path, const char *value);
int ulcr_read_text_file(const char *path, char *buffer, size_t buffer_size);
int ulcr_atomic_write_text_file(const char *path, const char *value);
int ulcr_copy_string(char *dst, size_t dst_size, const char *src);
int ulcr_timestamp_now(char *buffer, size_t buffer_size);
int ulcr_path_join(char *buffer, size_t buffer_size, const char *left, const char *right);
int ulcr_file_exists(const char *path);
int ulcr_is_numeric(const char *value);

#endif
