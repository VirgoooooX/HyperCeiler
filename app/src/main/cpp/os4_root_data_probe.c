#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <jni.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define READY_PATH "/data/local/tmp/hyperceiler_os4_watcher.ready"
#define LAUNCHER_COMM "com.miui.home"

/* Exact RELEASE-8.01.02.5334 Rust launcher targets. */
#define RUST_RVA_GET_CELL_COUNT_X      0x00785D0CULL
#define RUST_RVA_CURRENT_DEVICE_PARAM  0x01332500ULL

/* Unmodified prologue of DeviceConfigs::get_cell_count_x. */
static const uint8_t k_get_cell_count_x_signature[16] = {
    0xff, 0x83, 0x00, 0xd1,
    0xfd, 0x7b, 0x01, 0xa9,
    0xfd, 0x43, 0x00, 0x91,
    0x68, 0x5d, 0x00, 0xb0,
};

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void timestamp(char out[32]) {
    struct timespec ts;
    struct tm local_tm;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || localtime_r(&ts.tv_sec, &local_tm) == NULL) {
        snprintf(out, 32, "??:??:??.???");
        return;
    }
    snprintf(
        out,
        32,
        "%02d:%02d:%02d.%03ld",
        local_tm.tm_hour,
        local_tm.tm_min,
        local_tm.tm_sec,
        ts.tv_nsec / 1000000L
    );
}

static void log_stamp(const char *message) {
    char stamp[32];
    timestamp(stamp);
    printf("%s %s\n", stamp, message);
    fflush(stdout);
}

static bool numeric_name(const char *name) {
    if (name == NULL || *name == '\0') return false;
    for (const unsigned char *p = (const unsigned char *)name; *p != '\0'; ++p) {
        if (!isdigit(*p)) return false;
    }
    return true;
}

static bool read_exact(int fd, uint64_t address, void *buffer, size_t length) {
    ssize_t done = pread(fd, buffer, length, (off_t)address);
    return done == (ssize_t)length;
}

static bool write_exact(int fd, uint64_t address, const void *buffer, size_t length) {
    ssize_t done = pwrite(fd, buffer, length, (off_t)address);
    return done == (ssize_t)length;
}

static int open_mem(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", pid);
    return open(path, O_RDWR | O_CLOEXEC);
}

static bool process_exists(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d", pid);
    return access(path, F_OK) == 0;
}

static bool comm_is_launcher(pid_t pid) {
    char path[64];
    char comm[64] = {0};
    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    FILE *f = fopen(path, "re");
    if (f == NULL) return false;
    bool match = false;
    if (fgets(comm, sizeof(comm), f) != NULL) {
        char *nl = strchr(comm, '\n');
        if (nl != NULL) *nl = '\0';
        match = strcmp(comm, LAUNCHER_COMM) == 0;
    }
    fclose(f);
    return match;
}

static bool maps_mentions_launcher(pid_t pid) {
    char path[64];
    char line[4096];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE *f = fopen(path, "re");
    if (f == NULL) return false;
    bool match = false;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strstr(line, "com.miui.home") != NULL && strstr(line, "base.apk") != NULL) {
            match = true;
            break;
        }
    }
    fclose(f);
    return match;
}

static pid_t find_launcher_pid(bool deep_scan) {
    DIR *dir = opendir("/proc");
    if (dir == NULL) return 0;

    pid_t fallback = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!numeric_name(entry->d_name)) continue;
        pid_t pid = (pid_t)strtol(entry->d_name, NULL, 10);
        if (comm_is_launcher(pid)) {
            closedir(dir);
            return pid;
        }
        if (deep_scan && fallback == 0 && maps_mentions_launcher(pid)) {
            fallback = pid;
        }
    }
    closedir(dir);
    return fallback;
}

static bool rust_base_matches(int mem_fd, uint64_t base) {
    uint8_t signature[sizeof(k_get_cell_count_x_signature)];
    if (!read_exact(
            mem_fd,
            base + RUST_RVA_GET_CELL_COUNT_X,
            signature,
            sizeof(signature)
        )) {
        return false;
    }
    return memcmp(signature, k_get_cell_count_x_signature, sizeof(signature)) == 0;
}

static bool find_rust_base(pid_t pid, int mem_fd, uint64_t *base_out) {
    char maps_path[64];
    char line[8192];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
    FILE *maps = fopen(maps_path, "re");
    if (maps == NULL) return false;

    while (fgets(line, sizeof(line), maps) != NULL) {
        if (strstr(line, "base.apk") == NULL) continue;
        unsigned long long start = 0, end = 0, offset = 0;
        char perms[5] = {0};
        if (sscanf(line, "%llx-%llx %4s %llx", &start, &end, perms, &offset) != 4) continue;
        (void)end;
        (void)perms;
        (void)offset;

        uint8_t elf[4] = {0};
        if (!read_exact(mem_fd, (uint64_t)start, elf, sizeof(elf))) continue;
        if (!(elf[0] == 0x7f && elf[1] == 'E' && elf[2] == 'L' && elf[3] == 'F')) continue;
        if (rust_base_matches(mem_fd, (uint64_t)start)) {
            fclose(maps);
            *base_out = (uint64_t)start;
            return true;
        }
    }

    fclose(maps);
    return false;
}

static void put_i32_le(uint8_t *out, int value) {
    uint32_t v = (uint32_t)value;
    out[0] = (uint8_t)(v & 0xffU);
    out[1] = (uint8_t)((v >> 8) & 0xffU);
    out[2] = (uint8_t)((v >> 16) & 0xffU);
    out[3] = (uint8_t)((v >> 24) & 0xffU);
}

static int read_i32_le(const uint8_t *in) {
    uint32_t v = (uint32_t)in[0]
        | ((uint32_t)in[1] << 8)
        | ((uint32_t)in[2] << 16)
        | ((uint32_t)in[3] << 24);
    return (int32_t)v;
}

static bool write_xy(int mem_fd, uint64_t base, int cell_x, int cell_y) {
    uint8_t desired[8];
    uint8_t verify[8];
    put_i32_le(desired + 0, cell_x);
    put_i32_le(desired + 4, cell_y);
    if (!write_exact(mem_fd, base + RUST_RVA_CURRENT_DEVICE_PARAM, desired, sizeof(desired))) {
        return false;
    }
    return read_exact(
        mem_fd,
        base + RUST_RVA_CURRENT_DEVICE_PARAM,
        verify,
        sizeof(verify)
    ) && memcmp(desired, verify, sizeof(desired)) == 0;
}

static int pin_device_param(pid_t pid, uint64_t base, int cell_x, int cell_y) {
    int mem_fd = open_mem(pid);
    if (mem_fd < 0) {
        char message[192];
        snprintf(message, sizeof(message), "ERROR DATA probe open mem pid=%d errno=%d (%s)", pid, errno, strerror(errno));
        log_stamp(message);
        return 20;
    }

    if (kill(pid, SIGSTOP) != 0) {
        char message[192];
        snprintf(message, sizeof(message), "ERROR DATA probe SIGSTOP pid=%d errno=%d (%s)", pid, errno, strerror(errno));
        log_stamp(message);
        close(mem_fd);
        return 21;
    }

    uint8_t initial[16] = {0};
    bool initial_ok = read_exact(
        mem_fd,
        base + RUST_RVA_CURRENT_DEVICE_PARAM,
        initial,
        sizeof(initial)
    );
    int initial_x = initial_ok ? read_i32_le(initial + 0) : 0;
    int initial_y = initial_ok ? read_i32_le(initial + 4) : 0;
    int initial_min_x = initial_ok ? read_i32_le(initial + 8) : 0;
    int initial_min_y = initial_ok ? read_i32_le(initial + 12) : 0;

    bool initial_write_ok = write_xy(mem_fd, base, cell_x, cell_y);
    char message[256];
    snprintf(
        message,
        sizeof(message),
        "DATA initial CURRENT_DEVICE_PARAM %dx%d min=%d,%d -> %dx%d write=%s",
        initial_x,
        initial_y,
        initial_min_x,
        initial_min_y,
        cell_x,
        cell_y,
        initial_write_ok ? "ok" : "FAILED"
    );
    log_stamp(message);

    if (kill(pid, SIGCONT) != 0) {
        snprintf(message, sizeof(message), "ERROR DATA probe SIGCONT pid=%d errno=%d (%s)", pid, errno, strerror(errno));
        log_stamp(message);
        close(mem_fd);
        return 22;
    }

    const uint64_t deadline = monotonic_ms() + 4000ULL;
    int last_mismatch_x = INT32_MIN;
    int last_mismatch_y = INT32_MIN;
    int overwrite_count = 0;

    while (process_exists(pid) && monotonic_ms() < deadline) {
        uint8_t current[16] = {0};
        if (!read_exact(
                mem_fd,
                base + RUST_RVA_CURRENT_DEVICE_PARAM,
                current,
                sizeof(current)
            )) {
            break;
        }

        int current_x = read_i32_le(current + 0);
        int current_y = read_i32_le(current + 4);
        if (current_x != cell_x || current_y != cell_y) {
            int min_x = read_i32_le(current + 8);
            int min_y = read_i32_le(current + 12);
            if (current_x != last_mismatch_x || current_y != last_mismatch_y || overwrite_count < 4) {
                snprintf(
                    message,
                    sizeof(message),
                    "DATA overwrite observed %dx%d min=%d,%d -> re-pin %dx%d",
                    current_x,
                    current_y,
                    min_x,
                    min_y,
                    cell_x,
                    cell_y
                );
                log_stamp(message);
            }
            last_mismatch_x = current_x;
            last_mismatch_y = current_y;
            ++overwrite_count;
            if (!write_xy(mem_fd, base, cell_x, cell_y)) {
                snprintf(message, sizeof(message), "ERROR DATA re-pin verify failed pid=%d", pid);
                log_stamp(message);
                break;
            }
        }
        usleep(500);
    }

    uint8_t final_state[16] = {0};
    if (read_exact(
            mem_fd,
            base + RUST_RVA_CURRENT_DEVICE_PARAM,
            final_state,
            sizeof(final_state)
        )) {
        snprintf(
            message,
            sizeof(message),
            "DATA pin window done final=%dx%d min=%d,%d overwrites=%d",
            read_i32_le(final_state + 0),
            read_i32_le(final_state + 4),
            read_i32_le(final_state + 8),
            read_i32_le(final_state + 12),
            overwrite_count
        );
        log_stamp(message);
    } else {
        snprintf(message, sizeof(message), "DATA pin window ended; final read failed pid=%d", pid);
        log_stamp(message);
    }

    close(mem_fd);
    return 0;
}

static void mark_ready(void) {
    unlink(READY_PATH);
    int fd = open(READY_PATH, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;
    char text[64];
    int n = snprintf(text, sizeof(text), "%d\n", getpid());
    if (n > 0) (void)write(fd, text, (size_t)n);
    close(fd);
}

JNIEXPORT jint JNICALL
Java_com_sevtinge_hyperceiler_utils_os4_Os4LauncherRootPatcherCli_nativeWatch(
    JNIEnv *env,
    jclass clazz,
    jboolean hotseat_value,
    jboolean grid_value,
    jint cell_x_value,
    jint cell_y_value,
    jboolean icon_value,
    jint icon_size_value
) {
    (void)env;
    (void)clazz;
    (void)hotseat_value;
    (void)icon_value;
    (void)icon_size_value;

    setvbuf(stdout, NULL, _IOLBF, 0);

    const bool grid = grid_value == JNI_TRUE;
    const int cell_x = (int)cell_x_value;
    const int cell_y = (int)cell_y_value;

    char message[256];
    snprintf(
        message,
        sizeof(message),
        "native DATA probe ready: grid=%s cell=%dx%d pid=%d; instruction patches disabled",
        grid ? "true" : "false",
        cell_x,
        cell_y,
        getpid()
    );
    log_stamp(message);
    mark_ready();

    pid_t tracked = 0;
    bool pinned = false;
    unsigned int scan_counter = 0;

    for (;;) {
        bool deep_scan = (scan_counter++ % 4U) == 0U;
        pid_t pid = find_launcher_pid(deep_scan);
        if (pid <= 0) {
            if (tracked != 0) {
                snprintf(message, sizeof(message), "launcher pid=%d exited; probe re-armed", tracked);
                log_stamp(message);
                tracked = 0;
                pinned = false;
            }
            usleep(1000);
            continue;
        }

        if (pid != tracked) {
            tracked = pid;
            pinned = !grid;
            snprintf(message, sizeof(message), "launcher candidate detected pid=%d deep=%s", pid, deep_scan ? "true" : "false");
            log_stamp(message);
        }

        if (grid && !pinned) {
            int mem_fd = open_mem(pid);
            if (mem_fd >= 0) {
                uint64_t base = 0;
                if (find_rust_base(pid, mem_fd, &base)) {
                    close(mem_fd);
                    snprintf(message, sizeof(message), "Rust DATA target mapped pid=%d base=0x%llx", pid, (unsigned long long)base);
                    log_stamp(message);
                    int rc = pin_device_param(pid, base, cell_x, cell_y);
                    snprintf(message, sizeof(message), "DATA probe finished pid=%d rc=%d", pid, rc);
                    log_stamp(message);
                    pinned = true;
                } else {
                    close(mem_fd);
                }
            }
        }

        usleep(pinned ? 100000 : 500);
    }
}
