#ifndef ULCR_PROCFS_H
#define ULCR_PROCFS_H

#include "common.h"

typedef struct ulcr_region {
    uint64_t start;
    uint64_t end;
    uint64_t offset;
    int prot;
    int flags;
    int is_anonymous;
    int include;
    char name[ULCR_REGION_NAME];
} ulcr_region_t;

typedef struct ulcr_fd_entry {
    int fd;
    int flags;
    long long position;
    int supported;
    char target[ULCR_MAX_PATH];
} ulcr_fd_entry_t;

typedef struct ulcr_proc_layout {
    ulcr_region_t regions[ULCR_MAX_REGIONS];
    size_t region_count;
    ulcr_fd_entry_t fds[ULCR_MAX_FDS];
    size_t fd_count;
    char exe_path[ULCR_MAX_PATH];
    char cwd[ULCR_MAX_PATH];
} ulcr_proc_layout_t;

int ulcr_process_exists(pid_t pid);
int ulcr_collect_proc_layout(pid_t pid, ulcr_proc_layout_t *layout);
int ulcr_get_process_runtime_seconds(pid_t pid, double *seconds_out);

#endif
