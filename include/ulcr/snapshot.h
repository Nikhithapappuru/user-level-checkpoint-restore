#ifndef ULCR_SNAPSHOT_H
#define ULCR_SNAPSHOT_H

#include <sys/user.h>

#include "common.h"
#include "procfs.h"

typedef struct ulcr_snapshot_header {
    uint32_t magic;
    uint32_t version;
    pid_t pid;
    uint32_t region_count;
    uint32_t fd_count;
    struct user_regs_struct regs;
    char process_name[64];
    char exe_path[ULCR_MAX_PATH];
    char cwd[ULCR_MAX_PATH];
} ulcr_snapshot_header_t;

typedef struct ulcr_snapshot_region {
    ulcr_region_t meta;
    uint64_t file_offset;
    uint64_t data_size;
} ulcr_snapshot_region_t;

typedef struct ulcr_snapshot_image {
    ulcr_snapshot_header_t header;
    ulcr_snapshot_region_t regions[ULCR_MAX_REGIONS];
    ulcr_fd_entry_t fds[ULCR_MAX_FDS];
    size_t region_count;
    size_t fd_count;
} ulcr_snapshot_image_t;

int ulcr_capture_snapshot(pid_t pid, const char *process_name, const char *snapshot_dir);
int ulcr_restore_managed_process(
    const char *snapshot_dir,
    const char *log_path,
    const char *workload_path,
    const char *workload_args
);
int ulcr_write_manifest(const char *snapshot_dir, const ulcr_snapshot_image_t *image);
int ulcr_load_snapshot(const char *snapshot_dir, ulcr_snapshot_image_t *image, int *snapshot_fd);

#endif
