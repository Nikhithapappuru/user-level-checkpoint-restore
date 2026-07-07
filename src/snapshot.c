#define _GNU_SOURCE

#include "ulcr/snapshot.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

static int fsync_parent_directory(const char *path) {
    char dir_path[ULCR_MAX_PATH];
    char *slash = NULL;
    int dir_fd = -1;

    if (ulcr_copy_string(dir_path, sizeof(dir_path), path) != 0) {
        return -1;
    }
    slash = strrchr(dir_path, '/');
    if (slash == NULL) {
        return -1;
    }
    if (slash == dir_path) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }

    dir_fd = open(dir_path, O_RDONLY | O_DIRECTORY);
    if (dir_fd < 0) {
        return -1;
    }
    if (fsync(dir_fd) != 0) {
        close(dir_fd);
        return -1;
    }
    close(dir_fd);
    return 0;
}

static int ptrace_attach_and_stop(pid_t pid) {
    int status = 0;
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) != 0) {
        return -1;
    }
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    if (!WIFSTOPPED(status)) {
        return -1;
    }
    return 0;
}

static void ptrace_detach_if_needed(pid_t pid, int attached) {
    if (attached) {
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
    }
}

static ssize_t dump_region_bytes(pid_t pid, const ulcr_region_t *region, int out_fd) {
    uint64_t address = region->start;
    uint64_t remaining = region->end - region->start;
    char buffer[65536];
    ssize_t total = 0;
    while (remaining > 0) {
        struct iovec local_iov;
        struct iovec remote_iov;
        size_t chunk = remaining > sizeof(buffer) ? sizeof(buffer) : (size_t) remaining;
        ssize_t copied = 0;

        local_iov.iov_base = buffer;
        local_iov.iov_len = chunk;
        remote_iov.iov_base = (void *) (uintptr_t) address;
        remote_iov.iov_len = chunk;

        copied = process_vm_readv(pid, &local_iov, 1, &remote_iov, 1, 0);
        if (copied < 0) {
            return -1;
        }
        if (copied == 0) {
            return -1;
        }
        if (write(out_fd, buffer, (size_t) copied) != copied) {
            return -1;
        }
        total += copied;
        address += (uint64_t) copied;
        remaining -= (uint64_t) copied;
    }
    return total;
}

static int build_exec_argv(const char *workload_path, const char *workload_args, char **argv, size_t max_args) {
    static char args_copy[256];
    size_t argc = 0;
    char *token = NULL;

    if (max_args < 2) {
        return -1;
    }

    argv[argc++] = (char *) workload_path;
    args_copy[0] = '\0';
    if (workload_args != NULL) {
        ulcr_copy_string(args_copy, sizeof(args_copy), workload_args);
    }

    token = strtok(args_copy, " ");
    while (token != NULL && argc + 1 < max_args) {
        argv[argc++] = token;
        token = strtok(NULL, " ");
    }
    argv[argc] = NULL;
    return (int) argc;
}

int ulcr_write_manifest(const char *snapshot_dir, const ulcr_snapshot_image_t *image) {
    char path[ULCR_MAX_PATH];
    char tmp_path[ULCR_MAX_PATH];
    FILE *fp = NULL;
    size_t index = 0;

    snprintf(path, sizeof(path), "%s/manifest.txt", snapshot_dir);
    snprintf(tmp_path, sizeof(tmp_path), "%s/manifest.txt.tmp", snapshot_dir);
    fp = fopen(tmp_path, "w");
    if (fp == NULL) {
        return -1;
    }

    fprintf(fp, "process_name=%s\n", image->header.process_name);
    fprintf(fp, "pid=%d\n", image->header.pid);
    fprintf(fp, "exe_path=%s\n", image->header.exe_path);
    fprintf(fp, "cwd=%s\n", image->header.cwd);
    fprintf(fp, "region_count=%zu\n", image->region_count);
    fprintf(fp, "fd_count=%zu\n", image->fd_count);
    fprintf(fp, "saved_rip=0x%llx\n", (unsigned long long) image->header.regs.rip);
    fprintf(fp, "saved_rsp=0x%llx\n", (unsigned long long) image->header.regs.rsp);

    fprintf(fp, "\n[regions]\n");
    for (index = 0; index < image->region_count; ++index) {
        const ulcr_snapshot_region_t *region = &image->regions[index];
        fprintf(
            fp,
            "%03zu start=0x%llx end=0x%llx prot=%d include=%d name=%s size=%llu\n",
            index,
            (unsigned long long) region->meta.start,
            (unsigned long long) region->meta.end,
            region->meta.prot,
            region->meta.include,
            region->meta.name[0] == '\0' ? "<anonymous>" : region->meta.name,
            (unsigned long long) region->data_size
        );
    }

    fprintf(fp, "\n[fds]\n");
    for (index = 0; index < image->fd_count; ++index) {
        const ulcr_fd_entry_t *entry = &image->fds[index];
        fprintf(
            fp,
            "fd=%d supported=%d flags=%d pos=%lld target=%s\n",
            entry->fd,
            entry->supported,
            entry->flags,
            entry->position,
            entry->target
        );
    }

    fclose(fp);
    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    if (fsync_parent_directory(path) != 0) {
        return -1;
    }
    return 0;
}

int ulcr_capture_snapshot(pid_t pid, const char *process_name, const char *snapshot_dir) {
    ulcr_proc_layout_t layout;
    ulcr_snapshot_image_t image;
    char snapshot_path[ULCR_MAX_PATH];
    char snapshot_tmp_path[ULCR_MAX_PATH];
    int snapshot_fd = -1;
    int attached = 0;
    size_t included_regions = 0;
    size_t index = 0;

    memset(&layout, 0, sizeof(layout));
    memset(&image, 0, sizeof(image));

    if (ulcr_ensure_directory(snapshot_dir) != 0) {
        return -1;
    }
    if (ptrace_attach_and_stop(pid) != 0) {
        return -1;
    }
    attached = 1;

    if (ulcr_collect_proc_layout(pid, &layout) != 0) {
        ptrace_detach_if_needed(pid, attached);
        return -1;
    }
    if (ptrace(PTRACE_GETREGS, pid, NULL, &image.header.regs) != 0) {
        ptrace_detach_if_needed(pid, attached);
        return -1;
    }

    image.header.magic = ULCR_MAGIC;
    image.header.version = ULCR_VERSION;
    image.header.pid = pid;
    ulcr_copy_string(image.header.process_name, sizeof(image.header.process_name), process_name);
    ulcr_copy_string(image.header.exe_path, sizeof(image.header.exe_path), layout.exe_path);
    ulcr_copy_string(image.header.cwd, sizeof(image.header.cwd), layout.cwd);

    for (index = 0; index < layout.region_count; ++index) {
        image.regions[image.region_count].meta = layout.regions[index];
        ++image.region_count;
    }
    for (index = 0; index < layout.fd_count; ++index) {
        image.fds[image.fd_count++] = layout.fds[index];
    }
    image.header.region_count = (uint32_t) image.region_count;
    image.header.fd_count = (uint32_t) image.fd_count;

    snprintf(snapshot_path, sizeof(snapshot_path), "%s/snapshot.bin", snapshot_dir);
    snprintf(snapshot_tmp_path, sizeof(snapshot_tmp_path), "%s/snapshot.bin.tmp", snapshot_dir);
    snapshot_fd = open(snapshot_tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (snapshot_fd < 0) {
        ptrace_detach_if_needed(pid, attached);
        return -1;
    }

    if (write(snapshot_fd, &image.header, sizeof(image.header)) != (ssize_t) sizeof(image.header)) {
        close(snapshot_fd);
        ptrace_detach_if_needed(pid, attached);
        return -1;
    }
    if (write(snapshot_fd, image.regions, sizeof(ulcr_snapshot_region_t) * image.region_count) != (ssize_t) (sizeof(ulcr_snapshot_region_t) * image.region_count)) {
        close(snapshot_fd);
        ptrace_detach_if_needed(pid, attached);
        return -1;
    }
    if (write(snapshot_fd, image.fds, sizeof(ulcr_fd_entry_t) * image.fd_count) != (ssize_t) (sizeof(ulcr_fd_entry_t) * image.fd_count)) {
        close(snapshot_fd);
        ptrace_detach_if_needed(pid, attached);
        return -1;
    }

    for (index = 0; index < image.region_count; ++index) {
        ulcr_snapshot_region_t *region = &image.regions[index];
        off_t offset = 0;
        ssize_t bytes = 0;

        if (!region->meta.include) {
            continue;
        }
        offset = lseek(snapshot_fd, 0, SEEK_CUR);
        if (offset < 0) {
            close(snapshot_fd);
            ptrace_detach_if_needed(pid, attached);
            return -1;
        }
        bytes = dump_region_bytes(pid, &region->meta, snapshot_fd);
        if (bytes < 0) {
            region->meta.include = 0;
            region->file_offset = 0;
            region->data_size = 0;
            continue;
        }
        region->file_offset = (uint64_t) offset;
        region->data_size = (uint64_t) bytes;
        ++included_regions;
    }
    if (pwrite(snapshot_fd, &image.header, sizeof(image.header), 0) != (ssize_t) sizeof(image.header)) {
        close(snapshot_fd);
        ptrace_detach_if_needed(pid, attached);
        return -1;
    }
    if (pwrite(snapshot_fd, image.regions, sizeof(ulcr_snapshot_region_t) * image.region_count, sizeof(image.header)) != (ssize_t) (sizeof(ulcr_snapshot_region_t) * image.region_count)) {
        close(snapshot_fd);
        ptrace_detach_if_needed(pid, attached);
        return -1;
    }
    if (fsync(snapshot_fd) != 0) {
        close(snapshot_fd);
        ptrace_detach_if_needed(pid, attached);
        return -1;
    }
    close(snapshot_fd);
    if (rename(snapshot_tmp_path, snapshot_path) != 0) {
        unlink(snapshot_tmp_path);
        ptrace_detach_if_needed(pid, attached);
        return -1;
    }
    if (fsync_parent_directory(snapshot_path) != 0) {
        ptrace_detach_if_needed(pid, attached);
        return -1;
    }

    if (ulcr_write_manifest(snapshot_dir, &image) != 0) {
        ptrace_detach_if_needed(pid, attached);
        return -1;
    }
    ptrace_detach_if_needed(pid, attached);
    return included_regions > 0 ? 0 : -1;
}

int ulcr_load_snapshot(const char *snapshot_dir, ulcr_snapshot_image_t *image, int *snapshot_fd) {
    char snapshot_path[ULCR_MAX_PATH];

    memset(image, 0, sizeof(*image));
    snprintf(snapshot_path, sizeof(snapshot_path), "%s/snapshot.bin", snapshot_dir);
    *snapshot_fd = open(snapshot_path, O_RDONLY);
    if (*snapshot_fd < 0) {
        return -1;
    }
    if (read(*snapshot_fd, &image->header, sizeof(image->header)) != (ssize_t) sizeof(image->header)) {
        close(*snapshot_fd);
        return -1;
    }
    if (image->header.magic != ULCR_MAGIC || image->header.version != ULCR_VERSION) {
        close(*snapshot_fd);
        return -1;
    }
    image->region_count = image->header.region_count;
    image->fd_count = image->header.fd_count;
    if (image->region_count > ULCR_MAX_REGIONS || image->fd_count > ULCR_MAX_FDS) {
        close(*snapshot_fd);
        return -1;
    }
    if (read(*snapshot_fd, image->regions, sizeof(ulcr_snapshot_region_t) * image->region_count) != (ssize_t) (sizeof(ulcr_snapshot_region_t) * image->region_count)) {
        close(*snapshot_fd);
        return -1;
    }
    if (read(*snapshot_fd, image->fds, sizeof(ulcr_fd_entry_t) * image->fd_count) != (ssize_t) (sizeof(ulcr_fd_entry_t) * image->fd_count)) {
        close(*snapshot_fd);
        return -1;
    }
    return 0;
}

int ulcr_restore_managed_process(
    const char *snapshot_dir,
    const char *log_path,
    const char *workload_path,
    const char *workload_args
) {
    ulcr_snapshot_image_t image;
    int fd = -1;
    pid_t child = 0;

    if (ulcr_load_snapshot(snapshot_dir, &image, &fd) != 0) {
        return -1;
    }
    close(fd);

    child = fork();
    if (child < 0) {
        return -1;
    }
    if (child == 0) {
        char *argv[32];
        int log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (build_exec_argv(workload_path, workload_args, argv, 32) < 0) {
            _exit(126);
        }
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }
        setsid();
        setenv("ULCR_RESTORE_SNAPSHOT", snapshot_dir, 1);
        execv(workload_path, argv);
        _exit(127);
    }
    return (int) child;
}
