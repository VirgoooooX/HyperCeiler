#define _GNU_SOURCE
#include <jni.h>
#include <android/log.h>
#include <elf.h>
#include <link.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * HyperOS 4 System Launcher
 * RELEASE-8.01.02.5334-260807-08151151-R
 * libapp.so Build ID: 4f1bdaed80328aa4b22817f1e00300bf
 *
 * The first implementation modified Dart AOT text pages directly with
 * mprotect(RWX). Android 17 can reject writable+executable transitions, so
 * this revision uses LSPosed's Native Hook API instead. Build-ID and original
 * instruction checks are still kept before installing each inline hook.
 */

#define LOG_TAG "HyperCeilerOS4"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

typedef int (*HookFunType)(void *func, void *replace, void **backup);
typedef int (*UnhookFunType)(void *func);
typedef void (*NativeOnModuleLoaded)(const char *name, void *handle);

typedef struct {
    uint32_t version;
    HookFunType hook_func;
    UnhookFunType unhook_func;
} NativeAPIEntries;

static HookFunType g_hook_func = NULL;
static UnhookFunType g_unhook_func = NULL;

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

enum StatusBits {
    STATUS_NATIVE_API_READY = 1 << 0,
    STATUS_LIBAPP_FOUND     = 1 << 1,
    STATUS_BUILD_SUPPORTED  = 1 << 2,
    STATUS_HOTSEAT_HOOKED   = 1 << 3,
    STATUS_GRID_XMAX_HOOKED = 1 << 4,
    STATUS_GRID_XMIN_HOOKED = 1 << 5,
    STATUS_GRID_XDEF_HOOKED = 1 << 6,
    STATUS_GRID_YDEF_HOOKED = 1 << 7,
    STATUS_ICON_HOOKED      = 1 << 8,
    STATUS_LOAD_CALLBACK    = 1 << 9,
};

struct FindResult {
    uintptr_t base;
    bool found_name;
    bool supported;
};

static pthread_mutex_t g_install_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int g_status = 0;

static atomic_bool g_hotseat_enabled = false;
static atomic_bool g_grid_enabled = false;
static atomic_bool g_icon_enabled = false;
static atomic_int g_hotseat_max = 99;
static atomic_int g_cell_x = 4;
static atomic_int g_cell_y = 6;
static atomic_int g_icon_size = 182;

static bool g_hotseat_hooked = false;
static bool g_grid_xmax_hooked = false;
static bool g_grid_xmin_hooked = false;
static bool g_grid_xdef_hooked = false;
static bool g_grid_ydef_hooked = false;
static bool g_icon_hooked = false;

static void *g_hotseat_backup = NULL;
static void *g_grid_xmax_backup = NULL;
static void *g_grid_xmin_backup = NULL;
static void *g_grid_xdef_backup = NULL;
static void *g_grid_ydef_backup = NULL;
static void *g_icon_backup = NULL;

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
    result->base = (uintptr_t)info->dlpi_addr;
    result->supported = note_has_supported_build_id(info);
    return 1;
}

/*
 * Dart AOT target replacements.
 *
 * The target functions use the AArch64 return registers we already confirmed
 * from the supplied binary: raw integer in X0, tagged Smis in X0, and double
 * in D0. Extra Dart calling-convention arguments are intentionally ignored.
 */
static uintptr_t replacement_hotseat_max_count(void) {
    return (uintptr_t)atomic_load_explicit(&g_hotseat_max, memory_order_relaxed);
}

static inline uintptr_t dart_smi(int value) {
    return (uintptr_t)((uint64_t)(uint32_t)(value << 1));
}

static uintptr_t replacement_cell_count_x_max(void) {
    return dart_smi(9);
}

static uintptr_t replacement_cell_count_x_min(void) {
    return dart_smi(3);
}

static uintptr_t replacement_cell_count_x_def(void) {
    return dart_smi(atomic_load_explicit(&g_cell_x, memory_order_relaxed));
}

static uintptr_t replacement_cell_count_y_def(void) {
    return dart_smi(atomic_load_explicit(&g_cell_y, memory_order_relaxed));
}

static double replacement_icon_size(void) {
    return (double)atomic_load_explicit(&g_icon_size, memory_order_relaxed);
}

static bool verify_original(
    uintptr_t address,
    const uint8_t *expected,
    size_t expected_size,
    const char *name
) {
    if (memcmp((const void *)address, expected, expected_size) == 0) return true;
    LOGE("%s: prologue mismatch at %p; refusing hook", name, (void *)address);
    return false;
}

static bool install_one_hook(
    uintptr_t target,
    void *replacement,
    void **backup,
    const uint8_t *expected,
    size_t expected_size,
    const char *name
) {
    if (g_hook_func == NULL) {
        LOGE("%s: LSPosed native hook API is not ready", name);
        return false;
    }
    if (!verify_original(target, expected, expected_size, name)) return false;

    const int result = g_hook_func((void *)target, replacement, backup);
    if (result != 0) {
        LOGE("%s: hook_func failed with code %d", name, result);
        return false;
    }

    LOGI("%s: hooked target=%p replacement=%p", name, (void *)target, replacement);
    return true;
}

static void install_configured_hooks_at_base(uintptr_t base) {
    if (base == 0) return;

    pthread_mutex_lock(&g_install_lock);

    if (atomic_load_explicit(&g_hotseat_enabled, memory_order_relaxed) && !g_hotseat_hooked) {
        g_hotseat_hooked = install_one_hook(
            base + kRvaHotseatMaxCount,
            (void *)replacement_hotseat_max_count,
            &g_hotseat_backup,
            kExpectedHotseatPrologue,
            sizeof(kExpectedHotseatPrologue),
            "DeviceConfig.hotSeatMaxCount"
        );
        if (g_hotseat_hooked) atomic_fetch_or(&g_status, STATUS_HOTSEAT_HOOKED);
    }

    if (atomic_load_explicit(&g_grid_enabled, memory_order_relaxed)) {
        if (!g_grid_xmax_hooked) {
            g_grid_xmax_hooked = install_one_hook(
                base + kRvaCellCountXMax,
                (void *)replacement_cell_count_x_max,
                &g_grid_xmax_backup,
                kExpectedConfigGetterPrologue,
                sizeof(kExpectedConfigGetterPrologue),
                "DeviceConfig.cellCountXMax"
            );
            if (g_grid_xmax_hooked) atomic_fetch_or(&g_status, STATUS_GRID_XMAX_HOOKED);
        }
        if (!g_grid_xmin_hooked) {
            g_grid_xmin_hooked = install_one_hook(
                base + kRvaCellCountXMin,
                (void *)replacement_cell_count_x_min,
                &g_grid_xmin_backup,
                kExpectedConfigGetterPrologue,
                sizeof(kExpectedConfigGetterPrologue),
                "DeviceConfig.cellCountXMin"
            );
            if (g_grid_xmin_hooked) atomic_fetch_or(&g_status, STATUS_GRID_XMIN_HOOKED);
        }
        if (!g_grid_xdef_hooked) {
            g_grid_xdef_hooked = install_one_hook(
                base + kRvaCellCountXDef,
                (void *)replacement_cell_count_x_def,
                &g_grid_xdef_backup,
                kExpectedConfigGetterPrologue,
                sizeof(kExpectedConfigGetterPrologue),
                "DeviceConfig.cellCountXDef"
            );
            if (g_grid_xdef_hooked) atomic_fetch_or(&g_status, STATUS_GRID_XDEF_HOOKED);
        }
        if (!g_grid_ydef_hooked) {
            g_grid_ydef_hooked = install_one_hook(
                base + kRvaCellCountYDef,
                (void *)replacement_cell_count_y_def,
                &g_grid_ydef_backup,
                kExpectedConfigGetterPrologue,
                sizeof(kExpectedConfigGetterPrologue),
                "DeviceConfig.cellCountYDef"
            );
            if (g_grid_ydef_hooked) atomic_fetch_or(&g_status, STATUS_GRID_YDEF_HOOKED);
        }
    }

    if (atomic_load_explicit(&g_icon_enabled, memory_order_relaxed) && !g_icon_hooked) {
        g_icon_hooked = install_one_hook(
            base + kRvaIconSize,
            (void *)replacement_icon_size,
            &g_icon_backup,
            kExpectedIconSizePrologue,
            sizeof(kExpectedIconSizePrologue),
            "_IconConfig.getIconSize"
        );
        if (g_icon_hooked) atomic_fetch_or(&g_status, STATUS_ICON_HOOKED);
    }

    pthread_mutex_unlock(&g_install_lock);
}

static bool find_and_install_if_loaded(void) {
    struct FindResult result = {0};
    dl_iterate_phdr(find_libapp_callback, &result);

    if (!result.found_name) {
        LOGI("libapp.so is not mapped yet; waiting for LSPosed load callback");
        return true;
    }

    atomic_fetch_or(&g_status, STATUS_LIBAPP_FOUND);

    if (!result.supported) {
        LOGE("libapp.so found but Build ID is unsupported; refusing all hooks");
        return false;
    }

    atomic_fetch_or(&g_status, STATUS_BUILD_SUPPORTED);
    LOGI("supported libapp.so found at base=%p", (void *)result.base);
    install_configured_hooks_at_base(result.base);
    return true;
}

static void *install_after_load_worker(void *unused) {
    (void)unused;
    /*
     * Leave the linker callback before iterating program headers / invoking
     * inline hooks. This avoids doing non-trivial linker work while dlopen is
     * still unwinding.
     */
    usleep(1000);
    (void)find_and_install_if_loaded();
    return NULL;
}

static void schedule_install_after_load(void) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, install_after_load_worker, NULL) == 0) {
        pthread_detach(thread);
    } else {
        LOGE("failed to create post-load hook worker");
    }
}

static void on_library_loaded(const char *name, void *handle) {
    (void)handle;
    if (!ends_with_libapp(name)) return;

    atomic_fetch_or(&g_status, STATUS_LOAD_CALLBACK);
    LOGI("LSPosed native callback observed %s", name != NULL ? name : "libapp.so");
    schedule_install_after_load();
}

/*
 * LSPosed Modern Native Hook entrypoint. The library name is declared in
 * META-INF/xposed/native_init.list. System.loadLibrary from the Java bridge
 * loads this module inside com.miui.home; LSPosed calls native_init and gives
 * us its inline hook implementation.
 */
__attribute__((visibility("default"), used))
NativeOnModuleLoaded native_init(const NativeAPIEntries *entries) {
    if (entries == NULL || entries->hook_func == NULL) {
        LOGE("native_init called without hook_func");
        return on_library_loaded;
    }

    g_hook_func = entries->hook_func;
    g_unhook_func = entries->unhook_func;
    atomic_fetch_or(&g_status, STATUS_NATIVE_API_READY);
    LOGI(
        "native_init ready: API version=%u hook_func=%p unhook_func=%p",
        entries->version,
        (void *)g_hook_func,
        (void *)g_unhook_func
    );
    return on_library_loaded;
}

static bool configure_hotseat(int max_count) {
    if (max_count < 5 || max_count > 99) return false;
    atomic_store_explicit(&g_hotseat_max, max_count, memory_order_relaxed);
    atomic_store_explicit(&g_hotseat_enabled, true, memory_order_release);
    LOGI("hotseat requested: max=%d", max_count);
    return find_and_install_if_loaded();
}

static bool configure_grid(int cell_x, int cell_y) {
    if (cell_x < 3 || cell_x > 9 || cell_y < 4 || cell_y > 13) return false;
    atomic_store_explicit(&g_cell_x, cell_x, memory_order_relaxed);
    atomic_store_explicit(&g_cell_y, cell_y, memory_order_relaxed);
    atomic_store_explicit(&g_grid_enabled, true, memory_order_release);
    LOGI("grid requested: %dx%d", cell_x, cell_y);
    return find_and_install_if_loaded();
}

static bool configure_icon_size(int icon_size) {
    if (icon_size < 50 || icon_size > 360) return false;
    atomic_store_explicit(&g_icon_size, icon_size, memory_order_relaxed);
    atomic_store_explicit(&g_icon_enabled, true, memory_order_release);
    LOGI("icon size requested: %d", icon_size);
    return find_and_install_if_loaded();
}

JNIEXPORT jboolean JNICALL
Java_com_sevtinge_hyperceiler_libhook_utils_os4_Os4LauncherNativeBridge_nativePatchHotseat(
    JNIEnv *env, jclass clazz, jint max_count) {
    (void)env;
    (void)clazz;
    return configure_hotseat((int)max_count) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_sevtinge_hyperceiler_libhook_utils_os4_Os4LauncherNativeBridge_nativePatchGrid(
    JNIEnv *env, jclass clazz, jint cell_x, jint cell_y) {
    (void)env;
    (void)clazz;
    return configure_grid((int)cell_x, (int)cell_y) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_sevtinge_hyperceiler_libhook_utils_os4_Os4LauncherNativeBridge_nativePatchIconSize(
    JNIEnv *env, jclass clazz, jint icon_size) {
    (void)env;
    (void)clazz;
    return configure_icon_size((int)icon_size) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_sevtinge_hyperceiler_libhook_utils_os4_Os4LauncherNativeBridge_nativeGetStatus(
    JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    return (jint)atomic_load_explicit(&g_status, memory_order_acquire);
}
