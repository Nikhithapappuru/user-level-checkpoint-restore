#define _GNU_SOURCE

#include "ulcr/common.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int ulcr_fsync_path_directory(const char *path) {
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

int ulcr_copy_string(char *dst, size_t dst_size, const char *src) {
    if (dst == NULL || src == NULL || dst_size == 0) {
        return -1;
    }
    if (snprintf(dst, dst_size, "%s", src) >= (int) dst_size) {
        return -1;
    }
    return 0;
}

int ulcr_path_join(char *buffer, size_t buffer_size, const char *left, const char *right) {
    if (buffer == NULL || left == NULL || right == NULL || buffer_size == 0) {
        return -1;
    }
    if (snprintf(buffer, buffer_size, "%s/%s", left, right) >= (int) buffer_size) {
        return -1;
    }
    return 0;
}

int ulcr_ensure_directory(const char *path) {
    char tmp[ULCR_MAX_PATH];
    char *cursor = NULL;

    if (ulcr_copy_string(tmp, sizeof(tmp), path) != 0) {
        return -1;
    }

    for (cursor = tmp + 1; *cursor != '\0'; ++cursor) {
        if (*cursor == '/') {
            *cursor = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *cursor = '/';
        }
    }

    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

int ulcr_write_text_file(const char *path, const char *value) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    size_t len = 0;

    if (fd < 0) {
        return -1;
    }
    len = strlen(value);
    if (write(fd, value, len) != (ssize_t) len) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

int ulcr_read_text_file(const char *path, char *buffer, size_t buffer_size) {
    int fd = open(path, O_RDONLY);
    ssize_t bytes = 0;

    if (fd < 0) {
        return -1;
    }
    bytes = read(fd, buffer, buffer_size - 1);
    close(fd);
    if (bytes < 0) {
        return -1;
    }
    buffer[bytes] = '\0';
    return 0;
}

int ulcr_atomic_write_text_file(const char *path, const char *value) {
    char tmp_path[ULCR_MAX_PATH];
    int fd = -1;
    size_t len = 0;

    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path) >= (int) sizeof(tmp_path)) {
        return -1;
    }
    fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return -1;
    }
    len = strlen(value);
    if (write(fd, value, len) != (ssize_t) len) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    close(fd);
    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    if (ulcr_fsync_path_directory(path) != 0) {
        return -1;
    }
    return 0;
}

int ulcr_timestamp_now(char *buffer, size_t buffer_size) {
    time_t now = time(NULL);
    struct tm tm_now;

    if (localtime_r(&now, &tm_now) == NULL) {
        return -1;
    }
    if (strftime(buffer, buffer_size, "%Y%m%d-%H%M%S", &tm_now) == 0) {
        return -1;
    }
    return 0;
}

int ulcr_file_exists(const char *path) {
    return access(path, F_OK) == 0;
}

int ulcr_is_numeric(const char *value) {
    size_t index = 0;

    if (value == NULL || *value == '\0') {
        return 0;
    }
    for (index = 0; value[index] != '\0'; ++index) {
        if (!isdigit((unsigned char) value[index])) {
            return 0;
        }
    }
    return 1;
}
