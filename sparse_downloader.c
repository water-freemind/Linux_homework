#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MIN_LOGICAL_SIZE (128u * 1024u)
#define DEFAULT_CHUNK_SIZE 4096u
#define MAX_CHUNK_SIZE (1024u * 1024u)
#define MAX_PATH_BUFFER 4096u

static volatile sig_atomic_t stop_requested;

/* 信号处理函数只置位；文件同步和断点保存放在主流程安全地完成。 */
static void handle_sigint(int signo)
{
    (void)signo;
    stop_requested = 1;
}

static int write_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *p = buffer;
    size_t done = 0;
    while (done < length) {
        ssize_t n = write(fd, p + done, length - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) { errno = EIO; return -1; }
        done += (size_t)n;
    }
    return 0;
}

static int sync_data_retry(int fd)
{
    int rc;
    do { rc = fdatasync(fd); } while (rc < 0 && errno == EINTR);
    return rc;
}

static int sync_file_retry(int fd)
{
    int rc;
    do { rc = fsync(fd); } while (rc < 0 && errno == EINTR);
    return rc;
}

static int pread_all_at(int fd, void *buffer, size_t length, off_t offset)
{
    unsigned char *p = buffer;
    size_t done = 0;
    while (done < length) {
        ssize_t n = pread(fd, p + done, length - done, offset + (off_t)done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) { errno = EIO; return -1; }
        done += (size_t)n;
    }
    return 0;
}

static void make_sidecar_path(char *out, size_t out_size, const char *dest,
                              const char *suffix)
{
    (void)snprintf(out, out_size, "%s%s", dest, suffix);
}

/* 原子替换小型断点文件：先写临时文件并 fsync，再 rename。 */
static int save_checkpoint(const char *state_path, const struct stat *source,
                           uint64_t offset)
{
    char temp_path[MAX_PATH_BUFFER];
    char record[512];
    int n = snprintf(temp_path, sizeof(temp_path), "%s.tmp", state_path);
    if (n < 0 || (size_t)n >= sizeof(temp_path)) { errno = ENAMETOOLONG; return -1; }

    n = snprintf(record, sizeof(record),
                 "SPARSE-DOWNLOADER-V1\nsize=%" PRIuMAX "\nmtime=%" PRIdMAX ".%09ld\n"
                 "dev=%" PRIuMAX "\nino=%" PRIuMAX "\noffset=%" PRIu64 "\n",
                 (uintmax_t)source->st_size, (intmax_t)source->st_mtim.tv_sec,
                 source->st_mtim.tv_nsec, (uintmax_t)source->st_dev,
                 (uintmax_t)source->st_ino, offset);
    if (n < 0 || (size_t)n >= sizeof(record)) { errno = EOVERFLOW; return -1; }

    int fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    int rc = write_all(fd, record, (size_t)n);
    if (rc == 0) rc = sync_file_retry(fd);
    int saved_errno = errno;
    if (close(fd) < 0 && rc == 0) { rc = -1; saved_errno = errno; }
    if (rc == 0 && rename(temp_path, state_path) < 0) { rc = -1; saved_errno = errno; }
    if (rc == 0) {
        char parent[MAX_PATH_BUFFER];
        const char *slash = strrchr(state_path, '/');
        if (slash == NULL) strcpy(parent, ".");
        else if (slash == state_path) strcpy(parent, "/");
        else {
            size_t len = (size_t)(slash - state_path);
            if (len >= sizeof(parent)) { errno = ENAMETOOLONG; return -1; }
            memcpy(parent, state_path, len); parent[len] = '\0';
        }
        int dirfd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dirfd < 0) { rc = -1; saved_errno = errno; }
        else {
            if (sync_file_retry(dirfd) < 0) { rc = -1; saved_errno = errno; }
            close(dirfd);
        }
    }
    if (rc < 0) { unlink(temp_path); errno = saved_errno; }
    return rc;
}

static int parse_u64_line(const char *line, const char *key, uint64_t *value)
{
    size_t key_len = strlen(key);
    if (strncmp(line, key, key_len) != 0) return -1;
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(line + key_len, &end, 10);
    if (errno || end == line + key_len || (*end != '\n' && *end != '\0')) return -1;
    *value = (uint64_t)parsed;
    return 0;
}

static int load_checkpoint(const char *state_path, const struct stat *source,
                           uint64_t *offset)
{
    FILE *fp = fopen(state_path, "r");
    if (fp == NULL) return errno == ENOENT ? 1 : -1;
    char magic[64], size_line[96], mtime_line[128], dev_line[96], ino_line[96], off_line[96];
    int ok = fgets(magic, sizeof(magic), fp) && fgets(size_line, sizeof(size_line), fp) &&
             fgets(mtime_line, sizeof(mtime_line), fp) && fgets(dev_line, sizeof(dev_line), fp) &&
             fgets(ino_line, sizeof(ino_line), fp) && fgets(off_line, sizeof(off_line), fp);
    char extra[8];
    if (!ok || fgets(extra, sizeof(extra), fp) != NULL || strcmp(magic, "SPARSE-DOWNLOADER-V1\n") != 0) {
        fclose(fp); errno = EINVAL; return -1;
    }
    fclose(fp);

    uint64_t size, dev, ino, saved_offset;
    if (parse_u64_line(size_line, "size=", &size) || parse_u64_line(dev_line, "dev=", &dev) ||
        parse_u64_line(ino_line, "ino=", &ino) || parse_u64_line(off_line, "offset=", &saved_offset)) {
        errno = EINVAL; return -1;
    }
    char expected_mtime[128];
    (void)snprintf(expected_mtime, sizeof(expected_mtime), "mtime=%" PRIdMAX ".%09ld\n",
                   (intmax_t)source->st_mtim.tv_sec, source->st_mtim.tv_nsec);
    if (size != (uint64_t)source->st_size || dev != (uint64_t)source->st_dev ||
        ino != (uint64_t)source->st_ino || strcmp(mtime_line, expected_mtime) != 0 ||
        saved_offset > size) {
        errno = ESTALE; return -1;
    }
    *offset = saved_offset;
    return 0;
}

static void show_status(int fd, uint64_t logical_size, uint64_t completed)
{
    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat"); return; }
    uint64_t allocated = (uint64_t)st.st_blocks * 512u;
    double sparse_percent = logical_size == 0 ? 0.0 :
        (100.0 * (double)(logical_size > allocated ? logical_size - allocated : 0) / (double)logical_size);
    printf("进度：完成偏移=%" PRIu64 "/%" PRIu64 " 字节\n", completed, logical_size);
    printf("稀疏统计：逻辑大小=%" PRIu64 " 字节，实际占块=%" PRIu64
           " 字节，空洞比例=%.2f%%\n", logical_size, allocated, sparse_percent);
}

static void usage(const char *program)
{
    fprintf(stderr, "用法: %s SOURCE DEST [--chunk-size BYTES] [--delay-ms MS]\n", program);
    fprintf(stderr, "说明: DEST 不存在时创建；已有断点时续传；已有目标但无断点时拒绝覆盖。\n");
}

static int parse_positive(const char *s, uint64_t min, uint64_t max, uint64_t *out)
{
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno || end == s || *end != '\0' || v < min || v > max) return -1;
    *out = (uint64_t)v;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) { usage(argv[0]); return 2; }
    uint64_t chunk_size = DEFAULT_CHUNK_SIZE, delay_ms = 0;
    for (int i = 3; i < argc; i += 2) {
        uint64_t parsed;
        if (i + 1 >= argc) { usage(argv[0]); return 2; }
        if (strcmp(argv[i], "--chunk-size") == 0 && parse_positive(argv[i + 1], 512, MAX_CHUNK_SIZE, &parsed) == 0)
            chunk_size = parsed;
        else if (strcmp(argv[i], "--delay-ms") == 0 && parse_positive(argv[i + 1], 0, 5000, &parsed) == 0)
            delay_ms = parsed;
        else { usage(argv[0]); return 2; }
    }
    if (chunk_size % 512u != 0) { fprintf(stderr, "错误：块大小必须是512的整数倍。\n"); return 2; }

    int src_fd = open(argv[1], O_RDONLY | O_CLOEXEC);
    if (src_fd < 0) { perror("打开源文件"); return 1; }
    struct stat src_stat;
    if (fstat(src_fd, &src_stat) < 0 || !S_ISREG(src_stat.st_mode) || src_stat.st_size < (off_t)MIN_LOGICAL_SIZE) {
        fprintf(stderr, "错误：源文件必须是普通文件，且至少为128 KiB。\n"); close(src_fd); return 1;
    }

    int existing_dest = open(argv[2], O_RDONLY | O_CLOEXEC);
    int destination_exists = existing_dest >= 0;
    if (existing_dest >= 0) {
        struct stat dst_stat;
        if (fstat(existing_dest, &dst_stat) < 0 || (dst_stat.st_dev == src_stat.st_dev && dst_stat.st_ino == src_stat.st_ino)) {
            fprintf(stderr, "错误：源文件和目标文件不能是同一个文件。\n"); close(existing_dest); close(src_fd); return 1;
        }
        close(existing_dest);
    }

    char state_path[MAX_PATH_BUFFER];
    make_sidecar_path(state_path, sizeof(state_path), argv[2], ".state");
    uint64_t completed = 0;
    int state_rc = load_checkpoint(state_path, &src_stat, &completed);
    if (state_rc < 0) {
        perror("断点文件无效或源文件已变化"); close(src_fd); return 1;
    }
    int dst_fd;
    if (state_rc == 1) {
        if (destination_exists || access(argv[2], F_OK) == 0) {
            fprintf(stderr, "错误：目标文件已存在但没有有效断点；为避免覆盖，停止。\n");
            close(src_fd); return 1;
        }
        /* 先落盘 offset=0 的断点，再创建目标；崩溃后可安全从头开始。 */
        if (save_checkpoint(state_path, &src_stat, 0) < 0) { perror("创建断点"); close(src_fd); return 1; }
        dst_fd = open(argv[2], O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (dst_fd < 0) { perror("创建目标文件（已有文件不会覆盖）"); close(src_fd); return 1; }
        if (ftruncate(dst_fd, src_stat.st_size) < 0 || fsync(dst_fd) < 0) {
            perror("预分配逻辑文件大小"); close(dst_fd); close(src_fd); return 1;
        }
        printf("已创建稀疏目标文件：逻辑大小=%" PRIuMAX " 字节\n", (uintmax_t)src_stat.st_size);
    } else {
        dst_fd = open(argv[2], O_RDWR | O_CLOEXEC);
        if (dst_fd < 0) { perror("打开续传目标"); close(src_fd); return 1; }
        struct stat dst_stat;
        if (fstat(dst_fd, &dst_stat) < 0 || dst_stat.st_size != src_stat.st_size) {
            fprintf(stderr, "错误：目标文件大小与断点不匹配；为避免破坏数据，停止续传。\n");
            close(dst_fd); close(src_fd); return 1;
        }
        printf("检测到有效断点，从偏移=%" PRIu64 " 字节继续\n", completed);
    }

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_sigint;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);

    unsigned char *buffer = malloc((size_t)chunk_size);
    if (buffer == NULL) { perror("申请缓冲区"); close(dst_fd); close(src_fd); return 1; }
    uint64_t last_report = completed;
    while (completed < (uint64_t)src_stat.st_size) {
        /* 信号若恰好在上一块提交后的演示等待期间到达，就不再额外写下一块。 */
        if (stop_requested) {
            printf("收到 Ctrl-C：当前数据块已同步，断点已保存。\n");
            show_status(dst_fd, (uint64_t)src_stat.st_size, completed);
            free(buffer); close(dst_fd); close(src_fd); return 130;
        }
        size_t amount = (size_t)(((uint64_t)src_stat.st_size - completed) < chunk_size ?
                                 (uint64_t)src_stat.st_size - completed : chunk_size);
        if (pread_all_at(src_fd, buffer, amount, (off_t)completed) < 0) { perror("读取源文件"); goto fail; }
        /* 题目要求展示偏移写入：每块都先 lseek，再 write。 */
        if (lseek(dst_fd, (off_t)completed, SEEK_SET) < 0 || write_all(dst_fd, buffer, amount) < 0) {
            perror("按偏移写入目标文件"); goto fail;
        }
        if (sync_data_retry(dst_fd) < 0) { perror("同步目标数据"); goto fail; }
        completed += amount;
        if (save_checkpoint(state_path, &src_stat, completed) < 0) { perror("保存断点"); goto fail; }
        if (completed - last_report >= 64u * 1024u || completed == (uint64_t)src_stat.st_size) {
            printf("写入进度：%" PRIu64 "/%" PRIuMAX " 字节\n", completed, (uintmax_t)src_stat.st_size);
            fflush(stdout); last_report = completed;
        }
        if (stop_requested) {
            printf("收到 Ctrl-C：当前数据块已同步，断点已保存。\n");
            show_status(dst_fd, (uint64_t)src_stat.st_size, completed);
            free(buffer); close(dst_fd); close(src_fd); return 130;
        }
        if (delay_ms > 0) {
            struct timespec delay = { .tv_sec = (time_t)(delay_ms / 1000),
                                      .tv_nsec = (long)(delay_ms % 1000) * 1000000L };
            while (nanosleep(&delay, &delay) < 0 && errno == EINTR && !stop_requested) {}
        }
    }

    printf("传输完成。\n");
    show_status(dst_fd, (uint64_t)src_stat.st_size, completed);
    free(buffer); close(dst_fd); close(src_fd); return 0;

fail:
    free(buffer); close(dst_fd); close(src_fd); return 1;
}
