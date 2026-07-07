#define _GNU_SOURCE

#include "ulcr/procfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int parse_maps_line(const char *line, ulcr_region_t *region) {
    unsigned long start = 0;
    unsigned long end = 0;
    unsigned long offset = 0;
    char perms[8] = {0};
    char dev[32] = {0};
    unsigned long inode = 0;
    char path[ULCR_REGION_NAME] = {0};
    int parts = 0;

    parts = sscanf(
        line,
        "%lx-%lx %4s %lx %31s %lu %255[^\n]",
        &start,
        &end,
        perms,
        &offset,
        dev,
        &inode,
        path
    );
    if (parts < 6) {
        return -1;
    }

    memset(region, 0, sizeof(*region));
    region->start = start;
    region->end = end;
    region->offset = offset;
    if (perms[0] == 'r') {
        region->prot |= PROT_READ;
    }
    if (perms[1] == 'w') {
        region->prot |= PROT_WRITE;
    }
    if (perms[2] == 'x') {
        region->prot |= PROT_EXEC;
    }
    region->flags = (perms[3] == 'p') ? MAP_PRIVATE : MAP_SHARED;

    if (parts >= 7) {
        ulcr_copy_string(region->name, sizeof(region->name), path);
    } else {
        region->name[0] = '\0';
    }

    region->is_anonymous = (region->name[0] == '\0' || region->name[0] == '[');
    region->include = 0;
    if ((region->prot & PROT_READ) != 0) {
        if ((region->prot & PROT_WRITE) != 0 || strstr(region->name, "[heap]") != NULL || strstr(region->name, "[stack]") != NULL || region->name[0] == '\0') {
            region->include = 1;
        }
    }
    if (strcmp(region->name, "[vvar]") == 0 || strcmp(region->name, "[vdso]") == 0 || strcmp(region->name, "[vsyscall]") == 0) {
        region->include = 0;
    }
    return 0;
}

static int collect_regions(pid_t pid, ulcr_proc_layout_t *layout) {
    char maps_path[ULCR_MAX_PATH];
    FILE *fp = NULL;
    char line[1024];

    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
    fp = fopen(maps_path, "r");
    if (fp == NULL) {
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        ulcr_region_t region;
        if (layout->region_count >= ULCR_MAX_REGIONS) {
            break;
        }
        if (parse_maps_line(line, &region) == 0) {
            layout->regions[layout->region_count++] = region;
        }
    }
    fclose(fp);
    return 0;
}

static int collect_fdinfo(pid_t pid, ulcr_fd_entry_t *entry) {
    char fdinfo_path[ULCR_MAX_PATH];
    FILE *fp = NULL;
    char line[256];

    snprintf(fdinfo_path, sizeof(fdinfo_path), "/proc/%d/fdinfo/%d", pid, entry->fd);
    fp = fopen(fdinfo_path, "r");
    if (fp == NULL) {
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strncmp(line, "pos:", 4) == 0) {
            entry->position = atoll(line + 4);
        } else if (strncmp(line, "flags:", 6) == 0) {
            entry->flags = (int) strtol(line + 6, NULL, 8);
        }
    }
    fclose(fp);
    return 0;
}

static int collect_fds(pid_t pid, ulcr_proc_layout_t *layout) {
    char fd_root[ULCR_MAX_PATH];
    DIR *dir = NULL;
    struct dirent *de = NULL;

    snprintf(fd_root, sizeof(fd_root), "/proc/%d/fd", pid);
    dir = opendir(fd_root);
    if (dir == NULL) {
        return -1;
    }

    while ((de = readdir(dir)) != NULL) {
        char link_path[ULCR_MAX_PATH];
        char target[ULCR_MAX_PATH];
        ssize_t bytes = 0;
        ulcr_fd_entry_t *entry = NULL;

        if (!ulcr_is_numeric(de->d_name)) {
            continue;
        }
        if (layout->fd_count >= ULCR_MAX_FDS) {
            break;
        }

        entry = &layout->fds[layout->fd_count++];
        memset(entry, 0, sizeof(*entry));
        entry->fd = atoi(de->d_name);

        snprintf(link_path, sizeof(link_path), "%s/%s", fd_root, de->d_name);
        bytes = readlink(link_path, target, sizeof(target) - 1);
        if (bytes < 0) {
            continue;
        }
        target[bytes] = '\0';
        ulcr_copy_string(entry->target, sizeof(entry->target), target);
        entry->supported = (target[0] == '/') ? 1 : 0;
        collect_fdinfo(pid, entry);
    }
    closedir(dir);
    return 0;
}

static int collect_link_path(pid_t pid, const char *name, char *buffer, size_t buffer_size) {
    char path[ULCR_MAX_PATH];
    ssize_t bytes = 0;

    snprintf(path, sizeof(path), "/proc/%d/%s", pid, name);
    bytes = readlink(path, buffer, buffer_size - 1);
    if (bytes < 0) {
        return -1;
    }
    buffer[bytes] = '\0';
    return 0;
}

int ulcr_process_exists(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d", pid);
    return access(path, F_OK) == 0;
}

int ulcr_collect_proc_layout(pid_t pid, ulcr_proc_layout_t *layout) {
    memset(layout, 0, sizeof(*layout));
    if (collect_regions(pid, layout) != 0) {
        return -1;
    }
    if (collect_fds(pid, layout) != 0) {
        return -1;
    }
    if (collect_link_path(pid, "exe", layout->exe_path, sizeof(layout->exe_path)) != 0) {
        return -1;
    }
    if (collect_link_path(pid, "cwd", layout->cwd, sizeof(layout->cwd)) != 0) {
        return -1;
    }
    return 0;
}

int ulcr_get_process_runtime_seconds(pid_t pid, double *seconds_out) {
    char stat_path[ULCR_MAX_PATH];
    char uptime_path[] = "/proc/uptime";
    char buffer[4096];
    char *rest = NULL;
    char state = '\0';
    unsigned long long start_ticks = 0;
    double uptime_seconds = 0.0;
    FILE *fp = NULL;
    long ticks_per_second = 0;

    snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);
    if (ulcr_read_text_file(stat_path, buffer, sizeof(buffer)) != 0) {
        return -1;
    }
    rest = strrchr(buffer, ')');
    if (rest == NULL || rest[1] == '\0') {
        return -1;
    }
    ++rest;
    if (sscanf(
        rest,
        " %c %*d %*d %*d %*d %*d %*u %*lu %*lu %*lu %*lu %*lu %*lu %*ld %*ld %*ld %*ld %*ld %llu",
        &state,
        &start_ticks
    ) != 2) {
        return -1;
    }

    fp = fopen(uptime_path, "r");
    if (fp == NULL) {
        return -1;
    }
    if (fscanf(fp, "%lf", &uptime_seconds) != 1) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    ticks_per_second = sysconf(_SC_CLK_TCK);
    if (ticks_per_second <= 0) {
        return -1;
    }

    *seconds_out = uptime_seconds - ((double) start_ticks / (double) ticks_per_second);
    if (*seconds_out < 0.0) {
        *seconds_out = 0.0;
    }
    return 0;
}
