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
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define READY_PATH "/data/local/tmp/hyperceiler_os4_watcher.ready"
#define LAUNCHER_COMM "com.miui.home"

/* Exact RELEASE-8.01.02.5334 Rust launcher targets. */
#define RUST_RVA_SAVE_DEVICE_PARAM_PATCH 0x00785DE4ULL
#define RUST_RVA_CELL_X_MIN              0x00BC864CULL
#define RUST_RVA_CELL_X_MAX              0x00BC865CULL
#define RUST_RVA_CURRENT_DEVICE_PARAM    0x01332500ULL

/* Dart libapp.so targets that still need to be patched before Flutter consumes them. */
#define DART_RVA_BUILD_NOTE   0x000001C8ULL
#define DART_RVA_HOTSEAT_MAX  0x008F0500ULL
#define DART_RVA_ICON_SIZE    0x009AA734ULL
#define DART_KNOWN_APK_OFFSET 0x01CFC000ULL

static const uint8_t k_rust_save_expected[20] = {
    0x1f, 0x11, 0x00, 0xf1,
    0xa3, 0x06, 0x00, 0x54,
    0x08, 0x57, 0x00, 0xd0,
    0x08, 0x61, 0x05, 0x91,
    0xa9, 0x3c, 0x00, 0xb0,
};
static const uint8_t k_rust_x_min_expected[8] = {
    0x80, 0x00, 0x80, 0x52, 0xc0, 0x03, 0x5f, 0xd6,
};
static const uint8_t k_rust_x_max_expected[8] = {
    0xa0, 0x00, 0x80, 0x52, 0xc0, 0x03, 0x5f, 0xd6,
};
static const uint8_t k_dart_build_note[32] = {
    0x04, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x47, 0x4e, 0x55, 0x00,
    0x4f, 0x1b, 0xda, 0xed, 0x80, 0x32, 0x8a, 0xa4,
    0xb2, 0x28, 0x17, 0xf1, 0xe0, 0x03, 0x00, 0xbf,
};
static const uint8_t k_dart_hotseat_expected[8] = {
    0xfd, 0x79, 0xbf, 0xa9, 0xfd, 0x03, 0x0f, 0xaa,
};
static const uint8_t k_dart_icon_expected[12] = {
    0xfd, 0x79, 0xbf, 0xa9, 0xfd, 0x03, 0x0f, 0xaa,
    0xef, 0x81, 0x00, 0xd1,
};

static void put_u32_le(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)(value & 0xffU);
    out[1] = (uint8_t)((value >> 8) & 0xffU);
    out[2] = (uint8_t)((value >> 16) & 0xffU);
    out[3] = (uint8_t)((value >> 24) & 0xffU);
}

static void put_i32_le(uint8_t *out, int value) {
    put_u32_le(out, (uint32_t)value);
}

static void make_return_w0(int value, uint8_t out[8]) {
    uint32_t movz = 0x52800000U | (((uint32_t)value & 0xffffU) << 5);
    put_u32_le(out, movz);
    put_u32_le(out + 4, 0xd65f03c0U);
}

static void make_return_x0(int value, uint8_t out[8]) {
    uint32_t movz = 0xd2800000U | (((uint32_t)value & 0xffffU) << 5);
    put_u32_le(out, movz);
    put_u32_le(out + 4, 0xd65f03c0U);
}

static void make_return_double_d0(int value, uint8_t out[12]) {
    uint32_t movz = 0x52800000U | (((uint32_t)value & 0xffffU) << 5);
    put_u32_le(out, movz);
    put_u32_le(out + 4, 0x1e620000U);
    put_u32_le(out + 8, 0xd65f03c0U);
}

static void make_force_device_param_patch(int cell_x, int cell_y, uint8_t out[20]) {
    /*
     * 0x785de4 originally begins an optional logging path. Replace five
     * instructions with:
     *   mov w8, #cell_x
     *   str w8, [x0]
     *   mov w8, #cell_y
     *   str w8, [x0,#4]
     *   b   0x785eb8
     * The original function then reloads x0 from its stack slot and stores the
     * full DeviceParam, preserving cell_x_min/cell_y_min.
     */
    put_u32_le(out + 0, 0x52800008U | (((uint32_t)cell_x & 0xffffU) << 5));
    put_u32_le(out + 4, 0xb9000008U);
    put_u32_le(out + 8, 0x52800008U | (((uint32_t)cell_y & 0xffffU) << 5));
    put_u32_le(out + 12, 0xb9000408U);
    put_u32_le(out + 16, 0x14000031U);
}

static bool is_force_device_param_stub(const uint8_t current[20]) {
    uint32_t w0 = 0, w1 = 0, w2 = 0, w3 = 0, w4 = 0;
    memcpy(&w0, current + 0, 4);
    memcpy(&w1, current + 4, 4);
    memcpy(&w2, current + 8, 4);
    memcpy(&w3, current + 12, 4);
    memcpy(&w4, current + 16, 4);
    return (w0 & 0xffe0001fU) == 0x52800008U &&
           w1 == 0xb9000008U &&
           (w2 & 0xffe0001fU) == 0x52800008U &&
           w3 == 0xb9000408U &&
           w4 == 0x14000031U;
}

static bool is_return_w0_stub(const uint8_t current[8]) {
    uint32_t a = 0, b = 0;
    memcpy(&a, current, 4);
    memcpy(&b, current + 4, 4);
    return (a & 0xffe0001fU) == 0x52800000U && b == 0xd65f03c0U;
}

static bool is_return_x0_stub(const uint8_t current[8]) {
    uint32_t a = 0, b = 0;
    memcpy(&a, current, 4);
    memcpy(&b, current + 4, 4);
    return (a & 0xffe0001fU) == 0xd2800000U && b == 0xd65f03c0U;
}

static bool is_return_double_stub(const uint8_t current[12]) {
    uint32_t a = 0, b = 0, c = 0;
    memcpy(&a, current, 4);
    memcpy(&b, current + 4, 4);
    memcpy(&c, current + 8, 4);
    return (a & 0xffe0001fU) == 0x52800000U &&
           b == 0x1e620000U && c == 0xd65f03c0U;
}

static void print_hex(const uint8_t *data, size_t length) {
    for (size_t i = 0; i < length; ++i) printf("%02x", data[i]);
}

static bool read_exact(int fd, uint64_t address, void *buffer, size_t length) {
    ssize_t done = pread(fd, buffer, length, (off_t)address);
    return done == (ssize_t)length;
}

static bool write_exact(int fd, uint64_t address, const void *buffer, size_t length) {
    ssize_t done = pwrite(fd, buffer, length, (off_t)address);
    return done == (ssize_t)length;
}

static bool process_exists(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d", pid);
    return access(path, F_OK) == 0;
}

static bool numeric_name(const char *name) {
    if (name == NULL || *name == '\0') return false;
    for (const unsigned char *p = (const unsigned char *)name; *p != '\0'; ++p) {
        if (!isdigit(*p)) return false;
    }
    return true;
}

static pid_t find_launcher_pid(void) {
    DIR *dir = opendir("/proc");
    if (dir == NULL) return 0;
    pid_t result = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!numeric_name(entry->d_name)) continue;
        char path[96];
        snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
        FILE *f = fopen(path, "re");
        if (f == NULL) continue;
        char comm[64] = {0};
        if (fgets(comm, sizeof(comm), f) != NULL) {
            char *nl = strchr(comm, '\n');
            if (nl != NULL) *nl = '\0';
            if (strcmp(comm, LAUNCHER_COMM) == 0) {
                result = (pid_t)strtol(entry->d_name, NULL, 10);
                fclose(f);
                break;
            }
        }
        fclose(f);
    }
    closedir(dir);
    return result;
}

static int open_mem(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", pid);
    return open(path, O_RDWR | O_CLOEXEC);
}

static bool rust_signature_matches(int mem_fd, uint64_t base) {
    uint8_t save[20];
    uint8_t x_min[8];
    uint8_t x_max[8];
    if (!read_exact(mem_fd, base + RUST_RVA_SAVE_DEVICE_PARAM_PATCH, save, sizeof(save)) ||
        !read_exact(mem_fd, base + RUST_RVA_CELL_X_MIN, x_min, sizeof(x_min)) ||
        !read_exact(mem_fd, base + RUST_RVA_CELL_X_MAX, x_max, sizeof(x_max))) {
        return false;
    }
    const bool save_ok = memcmp(save, k_rust_save_expected, sizeof(save)) == 0 ||
                         is_force_device_param_stub(save);
    const bool min_ok = memcmp(x_min, k_rust_x_min_expected, sizeof(x_min)) == 0 ||
                        is_return_w0_stub(x_min);
    const bool max_ok = memcmp(x_max, k_rust_x_max_expected, sizeof(x_max)) == 0 ||
                        is_return_w0_stub(x_max);
    return save_ok && min_ok && max_ok;
}

static bool find_rust_launcher_base(pid_t pid, int mem_fd, uint64_t *base_out) {
    char maps_path[64];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
    FILE *maps = fopen(maps_path, "re");
    if (maps == NULL) return false;

    char line[8192];
    while (fgets(line, sizeof(line), maps) != NULL) {
        if (strstr(line, "base.apk") == NULL) continue;
        unsigned long long start = 0, end = 0, offset = 0;
        char perms[5] = {0};
        if (sscanf(line, "%llx-%llx %4s %llx", &start, &end, perms, &offset) != 4) continue;
        (void)end;
        (void)offset;
        uint8_t elf[4];
        if (!read_exact(mem_fd, (uint64_t)start, elf, sizeof(elf))) continue;
        if (!(elf[0] == 0x7f && elf[1] == 'E' && elf[2] == 'L' && elf[3] == 'F')) continue;
        if (rust_signature_matches(mem_fd, (uint64_t)start)) {
            fclose(maps);
            *base_out = (uint64_t)start;
            return true;
        }
    }
    fclose(maps);
    return false;
}

static int patch_checked(
    int mem_fd,
    const char *name,
    uint64_t address,
    const uint8_t *expected,
    const uint8_t *desired,
    size_t length,
    bool (*allowed_old)(const uint8_t *)
) {
    uint8_t current[32];
    uint8_t verify[32];
    if (length > sizeof(current)) return 70;
    if (!read_exact(mem_fd, address, current, length)) {
        printf("ERROR %s read addr=0x%llx errno=%d (%s)\n",
               name, (unsigned long long)address, errno, strerror(errno));
        return 71;
    }
    if (memcmp(current, desired, length) == 0) {
        printf("%s already desired addr=0x%llx\n", name, (unsigned long long)address);
        return 0;
    }
    if (memcmp(current, expected, length) != 0 &&
        (allowed_old == NULL || !allowed_old(current))) {
        printf("ERROR %s signature mismatch addr=0x%llx current=", name,
               (unsigned long long)address);
        print_hex(current, length);
        printf(" expected=");
        print_hex(expected, length);
        printf("\n");
        return 72;
    }
    if (!write_exact(mem_fd, address, desired, length)) {
        printf("ERROR %s write addr=0x%llx errno=%d (%s)\n",
               name, (unsigned long long)address, errno, strerror(errno));
        return 73;
    }
    if (!read_exact(mem_fd, address, verify, length) || memcmp(verify, desired, length) != 0) {
        printf("ERROR %s verify addr=0x%llx\n", name, (unsigned long long)address);
        return 74;
    }
    printf("%s patched addr=0x%llx bytes=", name, (unsigned long long)address);
    print_hex(desired, length);
    printf("\n");
    return 0;
}

static bool allow_old_rust_save(const uint8_t *value) {
    return is_force_device_param_stub(value);
}
static bool allow_old_return_w0(const uint8_t *value) {
    return is_return_w0_stub(value);
}
static bool allow_old_return_x0(const uint8_t *value) {
    return is_return_x0_stub(value);
}
static bool allow_old_return_double(const uint8_t *value) {
    return is_return_double_stub(value);
}

static int patch_rust_grid(pid_t pid, int cell_x, int cell_y) {
    int mem_fd = open_mem(pid);
    if (mem_fd < 0) return 20;
    uint64_t base = 0;
    if (!find_rust_launcher_base(pid, mem_fd, &base)) {
        close(mem_fd);
        return 10;
    }

    if (kill(pid, SIGSTOP) != 0) {
        close(mem_fd);
        return 21;
    }
    usleep(500);
    if (!process_exists(pid)) {
        close(mem_fd);
        return 22;
    }

    printf("RUST matched libapp_launcher.so pid=%d base=0x%llx\n",
           pid, (unsigned long long)base);

    uint8_t save_patch[20];
    uint8_t x_min_patch[8];
    uint8_t x_max_patch[8];
    make_force_device_param_patch(cell_x, cell_y, save_patch);
    make_return_w0(3, x_min_patch);
    make_return_w0(9, x_max_patch);

    int result = 0;
    int rc = patch_checked(mem_fd, "Rust.DeviceConfigs.save_device_param(force x/y)",
                           base + RUST_RVA_SAVE_DEVICE_PARAM_PATCH,
                           k_rust_save_expected, save_patch, sizeof(save_patch),
                           allow_old_rust_save);
    if (rc != 0 && result == 0) result = rc;

    rc = patch_checked(mem_fd, "Rust.LauncherCellCount.xMin",
                       base + RUST_RVA_CELL_X_MIN,
                       k_rust_x_min_expected, x_min_patch, sizeof(x_min_patch),
                       allow_old_return_w0);
    if (rc != 0 && result == 0) result = rc;

    rc = patch_checked(mem_fd, "Rust.LauncherCellCount.xMax",
                       base + RUST_RVA_CELL_X_MAX,
                       k_rust_x_max_expected, x_max_patch, sizeof(x_max_patch),
                       allow_old_return_w0);
    if (rc != 0 && result == 0) result = rc;

    uint8_t current_param[16] = {0};
    if (read_exact(mem_fd, base + RUST_RVA_CURRENT_DEVICE_PARAM,
                   current_param, sizeof(current_param))) {
        int old_x = 0, old_y = 0, min_x = 0, min_y = 0;
        memcpy(&old_x, current_param + 0, 4);
        memcpy(&old_y, current_param + 4, 4);
        memcpy(&min_x, current_param + 8, 4);
        memcpy(&min_y, current_param + 12, 4);
        put_i32_le(current_param + 0, cell_x);
        put_i32_le(current_param + 4, cell_y);
        if (write_exact(mem_fd, base + RUST_RVA_CURRENT_DEVICE_PARAM,
                        current_param, sizeof(current_param))) {
            printf("Rust.CURRENT_DEVICE_PARAM %dx%d min=%d,%d -> %dx%d (data write)\n",
                   old_x, old_y, min_x, min_y, cell_x, cell_y);
        } else {
            printf("WARNING Rust.CURRENT_DEVICE_PARAM write failed errno=%d (%s)\n",
                   errno, strerror(errno));
        }
    } else {
        printf("WARNING Rust.CURRENT_DEVICE_PARAM read failed errno=%d (%s)\n",
               errno, strerror(errno));
    }

    if (kill(pid, SIGCONT) != 0 && result == 0) result = 23;
    close(mem_fd);
    fflush(stdout);
    return result;
}

static bool verify_dart_build_note(int mem_fd, uint64_t base) {
    uint8_t note[sizeof(k_dart_build_note)];
    return read_exact(mem_fd, base + DART_RVA_BUILD_NOTE, note, sizeof(note)) &&
           memcmp(note, k_dart_build_note, sizeof(note)) == 0;
}

static bool find_dart_base(pid_t pid, int mem_fd, uint64_t *base_out) {
    char maps_path[64];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
    FILE *maps = fopen(maps_path, "re");
    if (maps == NULL) return false;
    char line[8192];
    uint64_t candidates[256];
    size_t count = 0;
    while (fgets(line, sizeof(line), maps) != NULL) {
        if (strstr(line, "base.apk") == NULL) continue;
        unsigned long long start = 0, end = 0, off = 0;
        char perms[5] = {0};
        if (sscanf(line, "%llx-%llx %4s %llx", &start, &end, perms, &off) != 4) continue;
        (void)end;
        if ((uint64_t)off == DART_KNOWN_APK_OFFSET &&
            verify_dart_build_note(mem_fd, (uint64_t)start)) {
            fclose(maps);
            *base_out = (uint64_t)start;
            return true;
        }
        if (count < 256) candidates[count++] = (uint64_t)start;
    }
    fclose(maps);
    for (size_t i = 0; i < count; ++i) {
        if (verify_dart_build_note(mem_fd, candidates[i])) {
            *base_out = candidates[i];
            return true;
        }
    }
    return false;
}

static int patch_dart_early(pid_t pid, bool hotseat, bool icon, int icon_size) {
    if (!hotseat && !icon) return 0;
    int mem_fd = open_mem(pid);
    if (mem_fd < 0) return 30;
    uint64_t base = 0;
    if (!find_dart_base(pid, mem_fd, &base)) {
        close(mem_fd);
        return 10;
    }
    if (kill(pid, SIGSTOP) != 0) {
        close(mem_fd);
        return 31;
    }
    usleep(500);
    printf("DART matched libapp.so pid=%d base=0x%llx\n",
           pid, (unsigned long long)base);

    int result = 0;
    if (hotseat) {
        uint8_t patch[8];
        make_return_x0(99, patch);
        int rc = patch_checked(mem_fd, "Dart.DeviceConfig.hotSeatMaxCount",
                               base + DART_RVA_HOTSEAT_MAX,
                               k_dart_hotseat_expected, patch, sizeof(patch),
                               allow_old_return_x0);
        if (rc != 0 && result == 0) result = rc;
    }
    if (icon) {
        uint8_t patch[12];
        make_return_double_d0(icon_size, patch);
        int rc = patch_checked(mem_fd, "Dart._IconConfig.getIconSize",
                               base + DART_RVA_ICON_SIZE,
                               k_dart_icon_expected, patch, sizeof(patch),
                               allow_old_return_double);
        if (rc != 0 && result == 0) result = rc;
    }

    if (kill(pid, SIGCONT) != 0 && result == 0) result = 32;
    close(mem_fd);
    fflush(stdout);
    return result;
}

static void mark_ready(void) {
    unlink(READY_PATH);
    int fd = open(READY_PATH, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        char text[64];
        int n = snprintf(text, sizeof(text), "%d\n", getpid());
        if (n > 0) (void)write(fd, text, (size_t)n);
        close(fd);
    }
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
    setvbuf(stdout, NULL, _IOLBF, 0);

    const bool hotseat = hotseat_value == JNI_TRUE;
    const bool grid = grid_value == JNI_TRUE;
    const bool icon = icon_value == JNI_TRUE;
    const int cell_x = (int)cell_x_value;
    const int cell_y = (int)cell_y_value;
    const int icon_size = (int)icon_size_value;

    printf("native resident watcher ready: hotseat=%s grid=%s cell=%dx%d icon=%s iconSize=%d pid=%d\n",
           hotseat ? "true" : "false", grid ? "true" : "false",
           cell_x, cell_y, icon ? "true" : "false", icon_size, getpid());
    mark_ready();

    pid_t tracked = 0;
    bool rust_done = false;
    bool dart_done = false;

    for (;;) {
        pid_t pid = find_launcher_pid();
        if (pid <= 0) {
            if (tracked != 0) {
                printf("launcher pid=%d exited; arming fast spawn watch\n", tracked);
                tracked = 0;
                rust_done = false;
                dart_done = false;
            }
            usleep(1000);
            continue;
        }

        if (pid != tracked) {
            tracked = pid;
            rust_done = !grid;
            dart_done = !hotseat && !icon;
            printf("launcher detected pid=%d\n", pid);
        }

        if (grid && !rust_done) {
            int rc = patch_rust_grid(pid, cell_x, cell_y);
            if (rc == 0) {
                rust_done = true;
                printf("RUST early grid patch complete pid=%d\n", pid);
            } else if (rc != 10 && rc != 20) {
                printf("ERROR RUST early grid patch pid=%d rc=%d\n", pid, rc);
                rust_done = true;
            }
        }

        if (!dart_done && process_exists(pid)) {
            int rc = patch_dart_early(pid, hotseat, icon, icon_size);
            if (rc == 0) {
                dart_done = true;
                printf("DART early patch complete pid=%d\n", pid);
            } else if (rc != 10 && rc != 30) {
                printf("ERROR DART early patch pid=%d rc=%d\n", pid, rc);
                dart_done = true;
            }
        }

        if (rust_done && dart_done) {
            usleep(100000);
        } else {
            usleep(500);
        }
    }
}
