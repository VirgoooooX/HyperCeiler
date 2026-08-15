#define _GNU_SOURCE
#include <jni.h>
#include <elf.h>
#include <link.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/*
 * HyperOS 4 System Launcher
 * RELEASE-8.01.02.5334-260807-08151151-R
 * libapp.so Build ID: 4f1bdaed80328aa4b22817f1e00300bf
 *
 * All RVAs below were recovered from libapp.so .gnu_debugdata.
 * Keep this table build-specific: never reuse these offsets for another OTA.
 */
static const uint8_t kSupportedBuildId[] = {
    0x4f, 0x1b, 0xda, 0xed, 0x80, 0x32, 0x8a, 0xa4,
    0xb2, 0x28, 0x17, 0xf1, 0xe0, 0x03, 0x00, 0xbf,
};

static const uintptr_t kRvaHotseatMaxCount = 0x008F0500u;
static const uintptr_t kRvaCellCountXMax    = 0x0095CA90u;
static const uintptr_t kRvaCellCountXMin    = 0x00978264u;
static const uintptr_t kRvaCellCountXDef    = 0x009A7CF8u;
static const uintptr_t kRvaIconSize         = 0x009AA734u;
static const uintptr_t kRvaCellCountYDef    = 0x00B57280u;

static const uint8_t kExpectedHotseatPrologue[8] = {
    0xfd, 0x79, 0xbf, 0xa9, 0xfd, 0x03, 0x0f, 0xaa,
};
static const uint8_t kExpectedConfigGetterPrologue[8] = {
    0x22, 0xf0, 0x41, 0xb8, 0x42, 0x80, 0x1c, 0x8b,
};
static const uint8_t kExpectedIconSizePrologue[12] = {
    0xfd, 0x79, 0xbf, 0xa9,
    0xfd, 0x03, 0x0f, 0xaa,
    0xef, 0x81, 0x00, 0xd1,
};

enum PatchKind {
    PATCH_HOTSEAT = 1,
    PATCH_GRID = 2,
    PATCH_ICON_SIZE = 3,
};

struct PatchRequest {
    enum PatchKind kind;
    int first;
    int second;
};

struct FindResult {
    uintptr_t base;
    bool found_name;
    bool supported;
};

static inline uintptr_t align4(uintptr_t value) {
    return (value + 3u) & ~(uintptr_t)3u;
}

static bool note_has_supported_build_id(const struct dl_phdr_info *info) {
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) *phdr = &info->dlpi_phdr[i];
        if (phdr->p_type != PT_NOTE || phdr->p_memsz < sizeof(Elf64_Nhdr)) continue;

        uintptr_t cursor = (uintptr_t)info->dlpi_addr + (uintptr_t)phdr->p_vaddr;
        const uintptr_t end = cursor + (uintptr_t)phdr->p_memsz;

        while (cursor + sizeof(Elf64_Nhdr) <= end) {
            const Elf64_Nhdr *note = (const Elf64_Nhdr *)cursor;
            cursor += sizeof(Elf64_Nhdr);

            const uintptr_t name_addr = cursor;
            const uintptr_t desc_addr = align4(name_addr + note->n_namesz);
            const uintptr_t next = align4(desc_addr + note->n_descsz);
            if (next > end || desc_addr > end) break;

            if (note->n_type == NT_GNU_BUILD_ID &&
                note->n_namesz >= 3 &&
                note->n_descsz == sizeof(kSupportedBuildId) &&
                memcmp((const void *)name_addr, "GNU", 3) == 0 &&
                memcmp((const void *)desc_addr, kSupportedBuildId, sizeof(kSupportedBuildId)) == 0) {
                return true;
            }
            cursor = next;
        }
    }
    return false;
}

static bool ends_with_libapp(const char *path) {
    if (path == NULL) return false;
    if (strcmp(path, "libapp.so") == 0) return true;
    static const char suffix[] = "/libapp.so";
    const size_t path_len = strlen(path);
    const size_t suffix_len = sizeof(suffix) - 1;
    if (path_len < suffix_len) return false;
    return memcmp(path + path_len - suffix_len, suffix, suffix_len) == 0;
}

static int find_libapp_callback(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    struct FindResult *result = (struct FindResult *)data;
    if (!ends_with_libapp(info->dlpi_name)) return 0;

    result->found_name = true;
    result->supported = note_has_supported_build_id(info);
    if (result->supported) result->base = (uintptr_t)info->dlpi_addr;
    return 1;
}

static bool patch_code(
    uintptr_t address,
    const void *desired,
    const void *expected,
    size_t length
) {
    if (length == 0) return false;
    if (memcmp((const void *)address, desired, length) == 0) return true;
    if (memcmp((const void *)address, expected, length) != 0) return false;

    const long page_size_long = sysconf(_SC_PAGESIZE);
    if (page_size_long <= 0) return false;
    const uintptr_t page_size = (uintptr_t)page_size_long;
    const uintptr_t first_page = address & ~(page_size - 1u);
    const uintptr_t end = address + length;
    const uintptr_t last_page_end = (end + page_size - 1u) & ~(page_size - 1u);
    const size_t protect_length = (size_t)(last_page_end - first_page);

    if (mprotect(
            (void *)first_page,
            protect_length,
            PROT_READ | PROT_WRITE | PROT_EXEC
        ) != 0) {
        return false;
    }

    memcpy((void *)address, desired, length);
    __builtin___clear_cache((char *)address, (char *)(address + length));
    return mprotect((void *)first_page, protect_length, PROT_READ | PROT_EXEC) == 0;
}

/* DeviceConfig.hotSeatMaxCount returns an unboxed native int in X0. */
static uint32_t movz_x0_int(int value) {
    const uint32_t imm = (uint32_t)value & 0xffffu;
    return 0xD2800000u | (imm << 5u); /* MOVZ X0, #imm16 */
}

static uint32_t movz_w0_int(int value) {
    const uint32_t imm = (uint32_t)value & 0xffffu;
    return 0x52800000u | (imm << 5u); /* MOVZ W0, #imm16 */
}

static bool patch_encoded_return(uintptr_t address, uint32_t encoded_value, const uint8_t expected[8]) {
    const uint32_t desired[2] = {
        movz_x0_int((int)encoded_value),
        0xD65F03C0u, /* RET */
    };
    return patch_code(address, desired, expected, sizeof(desired));
}

static uint32_t dart_smi(int value) {
    /* 64-bit Dart AOT with compressed pointers uses a one-bit Smi tag shift. */
    return (uint32_t)(value << 1);
}

static bool apply_hotseat_patch(uintptr_t base, int max_count) {
    if (max_count < 5 || max_count > 99) return false;
    return patch_encoded_return(
        base + kRvaHotseatMaxCount, (uint32_t)max_count, kExpectedHotseatPrologue
    );
}

static bool apply_grid_patch(uintptr_t base, int cell_x, int cell_y) {
    if (cell_x < 3 || cell_x > 9 || cell_y < 4 || cell_y > 13) return false;

    /*
     * These four DeviceConfig getters return tagged Dart ints (Smis), unlike
     * hotSeatMaxCount above. Returning a raw odd integer would be interpreted
     * as a heap object pointer and can crash the launcher.
     */
    bool ok = true;
    ok &= patch_encoded_return(base + kRvaCellCountXMax, dart_smi(9), kExpectedConfigGetterPrologue);
    ok &= patch_encoded_return(base + kRvaCellCountXMin, dart_smi(3), kExpectedConfigGetterPrologue);
    ok &= patch_encoded_return(base + kRvaCellCountXDef, dart_smi(cell_x), kExpectedConfigGetterPrologue);
    ok &= patch_encoded_return(base + kRvaCellCountYDef, dart_smi(cell_y), kExpectedConfigGetterPrologue);
    return ok;
}

static bool apply_icon_size_patch(uintptr_t base, int icon_size) {
    if (icon_size < 50 || icon_size > 360) return false;

    /*
     * _IconConfig.getIconSize returns an unboxed double in D0. HyperCeiler's
     * existing preference is a pixel-like integer (50..360), so convert the
     * immediate W0 value to double and return it directly:
     *   mov   w0, #icon_size
     *   scvtf d0, w0
     *   ret
     */
    const uint32_t desired[3] = {
        movz_w0_int(icon_size),
        0x1E620000u, /* SCVTF D0, W0 */
        0xD65F03C0u, /* RET */
    };
    return patch_code(
        base + kRvaIconSize,
        desired,
        kExpectedIconSizePrologue,
        sizeof(desired)
    );
}

static void *patch_worker(void *opaque) {
    struct PatchRequest *request = (struct PatchRequest *)opaque;

    /* Start from package-load time and wait until Flutter maps libapp.so. */
    for (int attempt = 0; attempt < 5000; ++attempt) {
        struct FindResult result = {0};
        dl_iterate_phdr(find_libapp_callback, &result);

        if (result.found_name) {
            if (result.supported && result.base != 0) {
                if (request->kind == PATCH_HOTSEAT) {
                    (void)apply_hotseat_patch(result.base, request->first);
                } else if (request->kind == PATCH_GRID) {
                    (void)apply_grid_patch(result.base, request->first, request->second);
                } else if (request->kind == PATCH_ICON_SIZE) {
                    (void)apply_icon_size_patch(result.base, request->first);
                }
            }
            free(request);
            return NULL;
        }
        usleep(2000); /* 2 ms, max ~10 s */
    }

    free(request);
    return NULL;
}

static bool enqueue_patch(enum PatchKind kind, int first, int second) {
    struct PatchRequest *request = (struct PatchRequest *)calloc(1, sizeof(*request));
    if (request == NULL) return false;
    request->kind = kind;
    request->first = first;
    request->second = second;

    pthread_t thread;
    if (pthread_create(&thread, NULL, patch_worker, request) != 0) {
        free(request);
        return false;
    }
    pthread_detach(thread);
    return true;
}

JNIEXPORT jboolean JNICALL
Java_com_sevtinge_hyperceiler_libhook_utils_os4_Os4LauncherNativeBridge_nativePatchHotseat(
    JNIEnv *env, jclass clazz, jint max_count) {
    (void)env;
    (void)clazz;
    return enqueue_patch(PATCH_HOTSEAT, (int)max_count, 0) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_sevtinge_hyperceiler_libhook_utils_os4_Os4LauncherNativeBridge_nativePatchGrid(
    JNIEnv *env, jclass clazz, jint cell_x, jint cell_y) {
    (void)env;
    (void)clazz;
    return enqueue_patch(PATCH_GRID, (int)cell_x, (int)cell_y) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_sevtinge_hyperceiler_libhook_utils_os4_Os4LauncherNativeBridge_nativePatchIconSize(
    JNIEnv *env, jclass clazz, jint icon_size) {
    (void)env;
    (void)clazz;
    return enqueue_patch(PATCH_ICON_SIZE, (int)icon_size, 0) ? JNI_TRUE : JNI_FALSE;
}
