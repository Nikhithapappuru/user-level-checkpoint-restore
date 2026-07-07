#define _GNU_SOURCE

#include "ulcr/runtime.h"
#include "ulcr/snapshot.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#define ULCR_MANAGED_BASE 0x530000000000ULL
#define ULCR_MANAGED_LIMIT 0x530000100000ULL

static int restore_supported_fds(const ulcr_snapshot_image_t *image) {
    size_t index = 0;
    for (index = 0; index < image->fd_count; ++index) {
        const ulcr_fd_entry_t *entry = &image->fds[index];
        int fd = -1;
        if (!entry->supported || entry->fd <= 2) {
            continue;
        }
        fd = open(entry->target, entry->flags, 0644);
        if (fd < 0) {
            continue;
        }
        if (entry->position > 0) {
            lseek(fd, entry->position, SEEK_SET);
        }
        if (fd != entry->fd) {
            dup2(fd, entry->fd);
            close(fd);
        }
    }
    return 0;
}

static int restore_managed_regions(const ulcr_snapshot_image_t *image, int snapshot_fd) {
    size_t index = 0;

    for (index = 0; index < image->region_count; ++index) {
        const ulcr_snapshot_region_t *region = &image->regions[index];
        uint64_t size = 0;
        void *mapped = NULL;

        if (!region->meta.include || region->data_size == 0) {
            continue;
        }
        if (region->meta.start < ULCR_MANAGED_BASE || region->meta.end > ULCR_MANAGED_LIMIT) {
            continue;
        }

        size = region->meta.end - region->meta.start;
        mapped = mmap(
            (void *) (uintptr_t) region->meta.start,
            (size_t) size,
            PROT_READ | PROT_WRITE,
            MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
            -1,
            0
        );
        if (mapped == MAP_FAILED) {
            return -1;
        }
        if (pread(snapshot_fd, mapped, (size_t) region->data_size, (off_t) region->file_offset) != (ssize_t) region->data_size) {
            return -1;
        }
        if (mprotect(mapped, (size_t) size, region->meta.prot) != 0) {
            return -1;
        }
    }
    return 0;
}

int ulcr_runtime_bootstrap(int argc, char **argv) {
    ulcr_snapshot_image_t image;
    const char *snapshot_dir = getenv("ULCR_RESTORE_SNAPSHOT");
    int snapshot_fd = -1;
    (void) argc;
    (void) argv;

    if (snapshot_dir == NULL || snapshot_dir[0] == '\0') {
        return 0;
    }

    if (ulcr_load_snapshot(snapshot_dir, &image, &snapshot_fd) != 0) {
        fprintf(stderr, "[ulcr-runtime] failed to load snapshot from %s\n", snapshot_dir);
        return -1;
    }
    chdir(image.header.cwd);
    restore_supported_fds(&image);
    if (restore_managed_regions(&image, snapshot_fd) != 0) {
        close(snapshot_fd);
        fprintf(stderr, "[ulcr-runtime] failed to restore managed regions\n");
        return -1;
    }
    close(snapshot_fd);
    setenv("ULCR_RESTORED", "1", 1);
    return 1;
}
