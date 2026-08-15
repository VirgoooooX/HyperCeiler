#define _FILE_OFFSET_BITS 64

#include <jni.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define RVA_BUILD_NOTE 0x000001C8ULL
#define RVA_HOTSEAT_MAX 0x008F0500ULL
#define RVA_CELL_X_MAX 0x0095CA90ULL
#define RVA_CELL_X_MIN 0x00978264ULL
#define RVA_CELL_X_DEF 0x009A7CF8ULL
#define RVA_ICON_SIZE 0x009AA734ULL
#define RVA_CELL_Y_DEF 0x00B57280ULL
#define KNOWN_LIBAPP_APK_OFFSET 0x01CFC000ULL

static const uint8_t k_build_note[32] = {
    0x04, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x47, 0x4e, 0x55, 0x00,
    0x4f, 0x1b, 0xda, 0xed, 0x80, 0x32, 0x8a, 0xa4,
    0xb2, 0x28, 0x17, 0xf1, 0xe0, 0x03, 0x00, 0xbf,
};

static const uint8_t k_expect_config_getter[8] = {
    0x22, 0xf0, 0x41, 0xb8, 0x42, 0x80, 0x1c, 0x8b,
};

static const uint8_t k_expect_hotseat[8] = {
    0xfd, 0x79, 0xbf, 0xa9, 0xfd, 0x03, 0x0f, 0xaa,
};

static const uint8_t k_expect_icon[12] = {
    0xfd, 0x79, 0xbf, 0xa9, 0xfd, 0x03, 0x0f, 0xaa,
    0xef, 0x81, 0x00, 0xd1,
};

static uint32_t get_u32_le(const uint8_t *in) {
    return (uint32_t)in[0]
        | ((uint32_t)in[1] << 8)
        | ((uint32_t)in[2] << 16)
        | ((uint32_t)in[3] << 24);
}

static void put_u32_le(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)(value & 0xffU);
    out[1] = (uint8_t)((value >> 8) & 0xffU);
    out[2] = (uint8_t)((value >> 16) & 0xffU);
    out[3] = (uint8_t)((value >> 24) & 0xffU);
}

static void make_return_int_x0(int value, uint8_t out[8]) {
    uint32_t movz = 0xd2800000U | (((uint32_t)value & 0xffffU) << 5);
    put_u32_le(out, movz);
    put_u32_le(out + 4, 0xd65f03c0U);
}

static void make_return_smi_x0(int value, uint8_t out[8]) {
    make_return_int_x0(value << 1, out);
}

static void make_return_double_d0(int value, uint8_t out[12]) {
    uint32_t movz = 0x52800000U | (((uint32_t)value & 0xffffU) << 5);
    put_u32_le(out, movz);
    put_u32_le(out + 4, 0x1e620000U);
    put_u32_le(out + 8, 0xd65f03c0U);
}

static bool is_prior_return_stub(const uint8_t *current, size_t length) {
    if (length == 8) {
        const uint32_t first = get_u32_le(current);
        const uint32_t second = get_u32_le(current + 4);
        return (first & 0xffe0001fU) == 0xd2800000U
            && second == 0xd65f03c0U;
    }
    if (length == 12) {
        const uint32_t first = get_u32_le(current);
        const uint32_t second = get_u32_le(current + 4);
        const uint32_t third = get_u32_le(current + 8);
        return (first & 0xffe0001fU) == 0x52800000U
            && second == 0x1e620000U
            && third == 0xd65f03c0U;
    }
    return false;
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

static bool verify_build_note(int mem_fd, uint64_t candidate) {
    uint8_t note[sizeof(k_build_note)];
    if (!read_exact(mem_fd, candidate + RVA_BUILD_NOTE, note, sizeof(note))) return false;
    return memcmp(note, k_build_note, sizeof(note)) == 0;
}

static bool find_libapp_base(pid_t pid, int mem_fd, uint64_t *base_out) {
    char maps_path[64];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
    FILE *maps = fopen(maps_path, "re");
    if (maps == NULL) {
        printf("ERROR maps open failed pid=%d errno=%d (%s)\n", pid, errno, strerror(errno));
        return false;
    }

    char line[8192];
    uint64_t fallback[256];
    size_t fallback_count = 0;
    while (fgets(line, sizeof(line), maps) != NULL) {
        unsigned long long start = 0, end = 0, offset = 0;
        char perms[5] = {0};
        char path[4096] = {0};
        int parsed = sscanf(line, "%llx-%llx %4s %llx %*s %*s %4095[^\n]",
                            &start, &end, perms, &offset, path);
        (void)end;
        (void)perms;
        if (parsed < 4 || strstr(line, "base.apk") == NULL) continue;
        if (fallback_count < (sizeof(fallback) / sizeof(fallback[0]))) {
            fallback[fallback_count++] = (uint64_t)start;
        }
        if ((uint64_t)offset == KNOWN_LIBAPP_APK_OFFSET
            && verify_build_note(mem_fd, (uint64_t)start)) {
            fclose(maps);
            *base_out = (uint64_t)start;
            printf("libapp.so matched known APK offset start=0x%llx\n", start);
            return true;
        }
    }
    fclose(maps);

    for (size_t i = 0; i < fallback_count; ++i) {
        if (verify_build_note(mem_fd, fallback[i])) {
            *base_out = fallback[i];
            printf("libapp.so matched Build ID scan start=0x%llx\n",
                   (unsigned long long)fallback[i]);
            return true;
        }
    }
    printf("WAIT libapp.so/build-id not ready pid=%d base.apk mappings=%zu\n",
           pid, fallback_count);
    return false;
}

static int patch_one(
    int mem_fd,
    const char *name,
    uint64_t address,
    const uint8_t *expected,
    const uint8_t *desired,
    size_t length
) {
    uint8_t current[16];
    uint8_t verify[16];
    if (length > sizeof(current)) {
        printf("ERROR %s internal length=%zu\n", name, length);
        return 30;
    }
    if (!read_exact(mem_fd, address, current, length)) {
        printf("ERROR %s read failed addr=0x%llx errno=%d (%s)\n",
               name, (unsigned long long)address, errno, strerror(errno));
        return 31;
    }
    if (memcmp(current, desired, length) == 0) {
        printf("%s already patched addr=0x%llx\n", name, (unsigned long long)address);
        return 0;
    }

    const bool original = memcmp(current, expected, length) == 0;
    const bool prior_stub = is_prior_return_stub(current, length);
    if (!original && !prior_stub) {
        printf("ERROR %s prologue mismatch addr=0x%llx current=",
               name, (unsigned long long)address);
        print_hex(current, length);
        printf(" expected=");
        print_hex(expected, length);
        printf("\n");
        return 32;
    }
    if (prior_stub) {
        printf("%s updating previous HyperCeiler return stub addr=0x%llx old=",
               name, (unsigned long long)address);
        print_hex(current, length);
        printf(" new=");
        print_hex(desired, length);
        printf("\n");
    }

    if (!write_exact(mem_fd, address, desired, length)) {
        printf("ERROR %s write failed addr=0x%llx errno=%d (%s)\n",
               name, (unsigned long long)address, errno, strerror(errno));
        return 33;
    }
    if (!read_exact(mem_fd, address, verify, length)) {
        printf("ERROR %s verify-read failed addr=0x%llx errno=%d (%s)\n",
               name, (unsigned long long)address, errno, strerror(errno));
        return 34;
    }
    if (memcmp(verify, desired, length) != 0) {
        printf("ERROR %s verify mismatch addr=0x%llx got=",
               name, (unsigned long long)address);
        print_hex(verify, length);
        printf(" wanted=");
        print_hex(desired, length);
        printf("\n");
        return 35;
    }
    printf("%s patched addr=0x%llx bytes=", name, (unsigned long long)address);
    print_hex(desired, length);
    printf("\n");
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_sevtinge_hyperceiler_utils_os4_Os4LauncherRootPatcherCli_nativePatch(
    JNIEnv *env,
    jclass clazz,
    jint pid_value,
    jboolean hotseat,
    jboolean grid,
    jint cell_x,
    jint cell_y,
    jboolean icon,
    jint icon_size
) {
    (void)env;
    (void)clazz;
#if !defined(__aarch64__)
    printf("ERROR native helper requires arm64-v8a\n");
    fflush(stdout);
    return 90;
#else
    pid_t pid = (pid_t)pid_value;
    char mem_path[64];
    snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", pid);
    printf("native patch start pid=%d hotseat=%s grid=%s cell=%dx%d icon=%s iconSize=%d\n",
           pid,
           hotseat == JNI_TRUE ? "true" : "false",
           grid == JNI_TRUE ? "true" : "false",
           (int)cell_x, (int)cell_y,
           icon == JNI_TRUE ? "true" : "false",
           (int)icon_size);
    fflush(stdout);

    int mem_fd = open(mem_path, O_RDWR | O_CLOEXEC);
    if (mem_fd < 0) {
        printf("ERROR mem open failed %s errno=%d (%s)\n", mem_path, errno, strerror(errno));
        fflush(stdout);
        return 11;
    }

    uint64_t base = 0;
    if (!find_libapp_base(pid, mem_fd, &base)) {
        close(mem_fd);
        fflush(stdout);
        return 10;
    }
    printf("supported libapp.so pid=%d base=0x%llx\n", pid, (unsigned long long)base);

    if (kill(pid, SIGSTOP) != 0) {
        printf("ERROR cannot SIGSTOP launcher pid=%d errno=%d (%s)\n", pid, errno, strerror(errno));
        close(mem_fd);
        fflush(stdout);
        return 12;
    }
    usleep(20000);

    int result = 0;
    uint8_t patch8[8];
    uint8_t patch12[12];

    if (hotseat == JNI_TRUE) {
        make_return_int_x0(99, patch8);
        int rc = patch_one(mem_fd, "DeviceConfig.hotSeatMaxCount",
                           base + RVA_HOTSEAT_MAX, k_expect_hotseat,
                           patch8, sizeof(patch8));
        if (rc != 0 && result == 0) result = rc;
    }

    if (grid == JNI_TRUE) {
        make_return_smi_x0(9, patch8);
        int rc = patch_one(mem_fd, "DeviceConfig.cellCountXMax",
                           base + RVA_CELL_X_MAX, k_expect_config_getter,
                           patch8, sizeof(patch8));
        if (rc != 0 && result == 0) result = rc;

        make_return_smi_x0(3, patch8);
        rc = patch_one(mem_fd, "DeviceConfig.cellCountXMin",
                       base + RVA_CELL_X_MIN, k_expect_config_getter,
                       patch8, sizeof(patch8));
        if (rc != 0 && result == 0) result = rc;

        make_return_smi_x0((int)cell_x, patch8);
        rc = patch_one(mem_fd, "DeviceConfig.cellCountXDef",
                       base + RVA_CELL_X_DEF, k_expect_config_getter,
                       patch8, sizeof(patch8));
        if (rc != 0 && result == 0) result = rc;

        make_return_smi_x0((int)cell_y, patch8);
        rc = patch_one(mem_fd, "DeviceConfig.cellCountYDef",
                       base + RVA_CELL_Y_DEF, k_expect_config_getter,
                       patch8, sizeof(patch8));
        if (rc != 0 && result == 0) result = rc;
    }

    if (icon == JNI_TRUE) {
        make_return_double_d0((int)icon_size, patch12);
        int rc = patch_one(mem_fd, "_IconConfig.getIconSize",
                           base + RVA_ICON_SIZE, k_expect_icon,
                           patch12, sizeof(patch12));
        if (rc != 0 && result == 0) result = rc;
    }

    if (kill(pid, SIGCONT) != 0) {
        printf("ERROR cannot SIGCONT launcher pid=%d errno=%d (%s)\n", pid, errno, strerror(errno));
        if (result == 0) result = 13;
    }
    close(mem_fd);
    printf("native patch finish pid=%d result=%d\n", pid, result);
    fflush(stdout);
    return result;
#endif
}
