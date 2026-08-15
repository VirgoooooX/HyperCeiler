#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <jni.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
#endif
#ifndef PTRACE_INTERRUPT
#define PTRACE_INTERRUPT 0x4207
#endif
#ifndef PTRACE_GETEVENTMSG
#define PTRACE_GETEVENTMSG 0x4201
#endif
#ifndef PTRACE_SETOPTIONS
#define PTRACE_SETOPTIONS 0x4200
#endif
#ifndef PTRACE_O_TRACESYSGOOD
#define PTRACE_O_TRACESYSGOOD 0x00000001
#endif
#ifndef PTRACE_O_TRACECLONE
#define PTRACE_O_TRACECLONE 0x00000008
#endif
#ifndef PTRACE_O_TRACEEXIT
#define PTRACE_O_TRACEEXIT 0x00000040
#endif
#ifndef PTRACE_EVENT_CLONE
#define PTRACE_EVENT_CLONE 3
#endif
#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif
#ifndef __WALL
#define __WALL 0x40000000
#endif

#define Java_com_sevtinge_hyperceiler_utils_os4_Os4LauncherRootPatcherCli_nativeWatch \
    Java_com_sevtinge_hyperceiler_utils_os4_Os4LauncherRootPatcherCli_nativeDataProbeUnusedV11
#include "os4_root_data_probe.c"
#undef Java_com_sevtinge_hyperceiler_utils_os4_Os4LauncherRootPatcherCli_nativeWatch

/* Exact RELEASE-8.01.02.5334-260807-08151151-R RVAs. */
#define RUST_RVA_SAVE_DEVICE_PARAM 0x00785DCCULL
#define RUST_RVA_CELL_X_MIN        0x00BC864CULL
#define RUST_RVA_CELL_X_MAX        0x00BC865CULL
#define RUST_RVA_GRID_SAVE_RETURN  0x006AF32CULL

#define GRID_SOURCE_X_ACTIVE  0x2cULL
#define GRID_SOURCE_X_DEFAULT 0x30ULL
#define GRID_SOURCE_Y         0x38ULL

#define EARLY_GATE_TIMEOUT_MS 2500ULL
#define TRACE_WINDOW_MS       2500ULL
#define MAX_TRACED_THREADS    256
#define A64_BRK_0             0xD4200000U

static const uint8_t k_save_signature[16] = {
    0xff, 0x03, 0x03, 0xd1,
    0xfd, 0x7b, 0x0b, 0xa9,
    0xfd, 0xc3, 0x02, 0x91,
    0x88, 0x5d, 0x00, 0xd0,
};
static const uint8_t k_x_min_expected[8] = {
    0x80, 0x00, 0x80, 0x52, 0xc0, 0x03, 0x5f, 0xd6,
};
static const uint8_t k_x_max_expected[8] = {
    0xa0, 0x00, 0x80, 0x52, 0xc0, 0x03, 0x5f, 0xd6,
};

struct hc_pt_regs_v11 {
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
};

struct hc_thread_v11 {
    pid_t tid;
    bool traced;
    bool stopped;
};

static struct hc_thread_v11 g_threads_v11[MAX_TRACED_THREADS];
static size_t g_threads_count_v11;

static struct hc_thread_v11 *hc_find_thread_v11(pid_t tid) {
    for (size_t i = 0; i < g_threads_count_v11; ++i) {
        if (g_threads_v11[i].tid == tid) return &g_threads_v11[i];
    }
    return NULL;
}

static struct hc_thread_v11 *hc_add_thread_v11(pid_t tid, bool traced, bool stopped) {
    struct hc_thread_v11 *thread = hc_find_thread_v11(tid);
    if (thread != NULL) {
        thread->traced = thread->traced || traced;
        thread->stopped = stopped;
        return thread;
    }
    if (g_threads_count_v11 >= MAX_TRACED_THREADS) return NULL;
    thread = &g_threads_v11[g_threads_count_v11++];
    memset(thread, 0, sizeof(*thread));
    thread->tid = tid;
    thread->traced = traced;
    thread->stopped = stopped;
    return thread;
}

static bool hc_tid_in_process_v11(pid_t pid, pid_t tid) {
    char path[96];
    snprintf(path, sizeof(path), "/proc/%d/task/%d", pid, tid);
    return access(path, F_OK) == 0;
}

static int hc_get_regs_v11(pid_t tid, struct hc_pt_regs_v11 *regs) {
    memset(regs, 0, sizeof(*regs));
    struct iovec iov = {regs, sizeof(*regs)};
    if (ptrace(PTRACE_GETREGSET, tid, (void *)(uintptr_t)NT_PRSTATUS, &iov) != 0)
        return -errno;
    return 0;
}

static int hc_poke_word_v11(pid_t tid, uint64_t address, uint64_t value) {
    errno = 0;
    if (ptrace(PTRACE_POKETEXT, tid, (void *)(uintptr_t)address,
               (void *)(uintptr_t)value) != 0)
        return -errno;
    return 0;
}

static int hc_read_word_v11(pid_t pid, uint64_t address, uint64_t *word_out) {
    int mem_fd = open_mem(pid);
    if (mem_fd < 0) return -errno;
    bool ok = read_exact(mem_fd, address, word_out, sizeof(*word_out));
    int err = errno;
    close(mem_fd);
    return ok ? 0 : -err;
}

static uint64_t hc_return_w0_word_v11(int value) {
    uint8_t bytes[8];
    uint32_t movz = 0x52800000U | (((uint32_t)value & 0xffffU) << 5);
    put_u32_le(bytes, movz);
    put_u32_le(bytes + 4, 0xd65f03c0U);
    uint64_t word = 0;
    memcpy(&word, bytes, sizeof(word));
    return word;
}

static int hc_patch_return_w0_v11(pid_t pid, pid_t writer, uint64_t address,
                                  const uint8_t expected[8], int value,
                                  const char *name) {
    uint64_t current = 0;
    int rc = hc_read_word_v11(pid, address, &current);
    if (rc != 0) return rc;

    uint64_t expected_word = 0;
    memcpy(&expected_word, expected, sizeof(expected_word));
    uint64_t desired = hc_return_w0_word_v11(value);
    if (current == desired) {
        char msg[192];
        snprintf(msg, sizeof(msg), "EARLY %s already desired addr=0x%llx", name,
                 (unsigned long long)address);
        log_stamp(msg);
        return 0;
    }
    if (current != expected_word) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "ERROR EARLY %s signature addr=0x%llx current=0x%016llx expected=0x%016llx",
                 name, (unsigned long long)address,
                 (unsigned long long)current, (unsigned long long)expected_word);
        log_stamp(msg);
        return -EINVAL;
    }

    rc = hc_poke_word_v11(writer, address, desired);
    if (rc != 0) return rc;
    uint64_t verify = 0;
    rc = hc_read_word_v11(pid, address, &verify);
    if (rc != 0 || verify != desired) return -EIO;

    char msg[192];
    snprintf(msg, sizeof(msg), "EARLY %s patched addr=0x%llx value=%d",
             name, (unsigned long long)address, value);
    log_stamp(msg);
    return 0;
}

static void hc_reset_threads_v11(void) {
    memset(g_threads_v11, 0, sizeof(g_threads_v11));
    g_threads_count_v11 = 0;
}

/*
 * Catch the launcher before its first Rust instruction. We seize only the
 * process leader, then advance it syscall-by-syscall. At the mmap syscall
 * exit where libapp_launcher.so becomes visible, the leader is still in a
 * ptrace stop, so no Rust user-space instruction after that mmap has run yet.
 */
static int hc_gate_until_rust_map_v11(pid_t pid, uint64_t *base_out) {
    hc_reset_threads_v11();
    const long options = PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEEXIT;
    if (ptrace(PTRACE_SEIZE, pid, 0, (void *)(uintptr_t)options) != 0)
        return -errno;
    hc_add_thread_v11(pid, true, false);

    if (ptrace(PTRACE_INTERRUPT, pid, 0, 0) != 0) return -errno;
    int status = 0;
    if (waitpid(pid, &status, __WALL) != pid || !WIFSTOPPED(status)) return -EIO;
    hc_find_thread_v11(pid)->stopped = true;

    char msg[192];
    snprintf(msg, sizeof(msg), "EARLY syscall gate seized pid=%d", pid);
    log_stamp(msg);

    uint64_t start = monotonic_ms();
    int stops = 0;
    int pending_signal = 0;
    while (process_exists(pid) && monotonic_ms() - start < EARLY_GATE_TIMEOUT_MS) {
        int mem_fd = open_mem(pid);
        if (mem_fd >= 0) {
            uint64_t base = 0;
            bool mapped = find_rust_base(pid, mem_fd, &base);
            close(mem_fd);
            if (mapped) {
                *base_out = base;
                snprintf(msg, sizeof(msg),
                         "EARLY rust map caught pid=%d base=0x%llx syscall_stops=%d elapsed=%llums",
                         pid, (unsigned long long)base, stops,
                         (unsigned long long)(monotonic_ms() - start));
                log_stamp(msg);
                return 0;
            }
        }

        if (ptrace(PTRACE_SYSCALL, pid, 0, (void *)(uintptr_t)pending_signal) != 0)
            return -errno;
        hc_find_thread_v11(pid)->stopped = false;
        pending_signal = 0;

        status = 0;
        if (waitpid(pid, &status, __WALL) != pid) {
            if (errno == EINTR) continue;
            return -errno;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) return -ESRCH;
        if (!WIFSTOPPED(status)) continue;
        hc_find_thread_v11(pid)->stopped = true;
        ++stops;

        int sig = WSTOPSIG(status);
        if (sig != (SIGTRAP | 0x80) && sig != SIGTRAP && sig != SIGSTOP)
            pending_signal = sig;
    }
    return -ETIMEDOUT;
}

static int hc_seize_rest_and_stop_v11(pid_t pid) {
    char task_path[64];
    snprintf(task_path, sizeof(task_path), "/proc/%d/task", pid);
    DIR *dir = opendir(task_path);
    if (dir == NULL) return -errno;

    pid_t tids[MAX_TRACED_THREADS];
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < MAX_TRACED_THREADS) {
        if (numeric_name(entry->d_name))
            tids[count++] = (pid_t)strtol(entry->d_name, NULL, 10);
    }
    closedir(dir);

    const long options = PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXIT;
    for (size_t i = 0; i < count; ++i) {
        pid_t tid = tids[i];
        struct hc_thread_v11 *known = hc_find_thread_v11(tid);
        if (known != NULL && known->traced) {
            (void)ptrace(PTRACE_SETOPTIONS, tid, 0, (void *)(uintptr_t)options);
            continue;
        }
        if (ptrace(PTRACE_SEIZE, tid, 0, (void *)(uintptr_t)options) != 0) {
            if (errno != ESRCH) {
                char msg[192];
                snprintf(msg, sizeof(msg), "ERROR EARLY PTRACE_SEIZE tid=%d errno=%d (%s)",
                         tid, errno, strerror(errno));
                log_stamp(msg);
            }
            continue;
        }
        hc_add_thread_v11(tid, true, false);
    }

    for (size_t i = 0; i < g_threads_count_v11; ++i) {
        struct hc_thread_v11 *thread = &g_threads_v11[i];
        if (!thread->traced || thread->stopped) continue;
        if (ptrace(PTRACE_INTERRUPT, thread->tid, 0, 0) != 0) continue;
        int status = 0;
        if (waitpid(thread->tid, &status, __WALL) == thread->tid && WIFSTOPPED(status))
            thread->stopped = true;
    }
    return g_threads_count_v11 > 0 ? 0 : -ESRCH;
}

static pid_t hc_any_stopped_v11(void) {
    for (size_t i = 0; i < g_threads_count_v11; ++i) {
        if (g_threads_v11[i].traced && g_threads_v11[i].stopped)
            return g_threads_v11[i].tid;
    }
    return 0;
}

static void hc_continue_all_v11(void) {
    for (size_t i = 0; i < g_threads_count_v11; ++i) {
        struct hc_thread_v11 *thread = &g_threads_v11[i];
        if (!thread->traced || !thread->stopped) continue;
        if (ptrace(PTRACE_CONT, thread->tid, 0, 0) == 0)
            thread->stopped = false;
    }
}

static bool hc_save_signature_matches_v11(int mem_fd, uint64_t base) {
    uint8_t current[sizeof(k_save_signature)];
    return read_exact(mem_fd, base + RUST_RVA_SAVE_DEVICE_PARAM,
                      current, sizeof(current)) &&
           memcmp(current, k_save_signature, sizeof(current)) == 0;
}

static bool hc_write_i32_v11(int mem_fd, uint64_t address, int value) {
    uint8_t bytes[4];
    put_i32_le(bytes, value);
    if (!write_exact(mem_fd, address, bytes, sizeof(bytes))) return false;
    uint8_t verify[4];
    return read_exact(mem_fd, address, verify, sizeof(verify)) &&
           memcmp(bytes, verify, sizeof(bytes)) == 0;
}

static int hc_mutate_param_v11(pid_t pid, pid_t tid, uint64_t base, uint64_t bp,
                               int cell_x, int cell_y,
                               int *old_x_out, int *old_y_out,
                               int *min_x_out, int *min_y_out,
                               bool *grid_source_out) {
    struct hc_pt_regs_v11 regs;
    int rc = hc_get_regs_v11(tid, &regs);
    if (rc != 0) return rc;
    if (regs.pc != bp) return 1;

    uint64_t param = regs.regs[0];
    uint64_t lr = regs.regs[30];
    uint64_t caller_rva = lr >= base ? lr - base : 0;
    bool grid_source = caller_rva == RUST_RVA_GRID_SAVE_RETURN;

    int mem_fd = open_mem(pid);
    if (mem_fd < 0) return -errno;
    uint8_t value[16] = {0};
    if (!read_exact(mem_fd, param, value, sizeof(value))) {
        rc = -errno;
        close(mem_fd);
        return rc;
    }

    int old_x = read_i32_le(value + 0);
    int old_y = read_i32_le(value + 4);
    int min_x = read_i32_le(value + 8);
    int min_y = read_i32_le(value + 12);
    put_i32_le(value + 0, cell_x);
    put_i32_le(value + 4, cell_y);

    bool ok = write_exact(mem_fd, param, value, 8);
    uint8_t verify[8] = {0};
    ok = ok && read_exact(mem_fd, param, verify, sizeof(verify)) &&
         memcmp(verify, value, sizeof(verify)) == 0;

    bool source_ok = true;
    uint64_t source = regs.regs[19];
    if (grid_source) {
        /*
         * GridConfig::instance builds a temporary DeviceParam, calls
         * save_device_param, then immediately reloads x from [x19+0x2c] and
         * y from [x19+0x38]. Mutating only x0 changes persistence but not the
         * GridConfig being constructed. Patch those caller-owned fields while
         * the BRK still has the caller paused.
         */
        source_ok = hc_write_i32_v11(mem_fd, source + GRID_SOURCE_X_ACTIVE, cell_x) &&
                    hc_write_i32_v11(mem_fd, source + GRID_SOURCE_X_DEFAULT, cell_x) &&
                    hc_write_i32_v11(mem_fd, source + GRID_SOURCE_Y, cell_y);
    }
    close(mem_fd);

    if (old_x_out) *old_x_out = old_x;
    if (old_y_out) *old_y_out = old_y;
    if (min_x_out) *min_x_out = min_x;
    if (min_y_out) *min_y_out = min_y;
    if (grid_source_out) *grid_source_out = grid_source;

    char msg[640];
    snprintf(msg, sizeof(msg),
             "SWBP hit tid=%d pc=0x%llx lr=0x%llx caller_rva=0x%llx x0=0x%llx x19=0x%llx DeviceParam %dx%d min=%d,%d -> %dx%d write=%s grid_source=%s source_write=%s",
             tid, (unsigned long long)regs.pc, (unsigned long long)lr,
             (unsigned long long)caller_rva, (unsigned long long)param,
             (unsigned long long)source, old_x, old_y, min_x, min_y,
             cell_x, cell_y, ok ? "ok" : "FAILED",
             grid_source ? "true" : "false", source_ok ? "ok" : "FAILED");
    log_stamp(msg);
    return ok && source_ok ? 0 : -EIO;
}

static int hc_step_original_v11(pid_t tid, uint64_t bp,
                                uint64_t original_word, uint64_t brk_word,
                                bool rearm) {
    int rc = hc_poke_word_v11(tid, bp, original_word);
    if (rc != 0) return rc;
    if (ptrace(PTRACE_SINGLESTEP, tid, 0, 0) != 0) return -errno;
    int status = 0;
    if (waitpid(tid, &status, __WALL) != tid || !WIFSTOPPED(status)) return -EIO;
    struct hc_thread_v11 *thread = hc_find_thread_v11(tid);
    if (thread != NULL) thread->stopped = true;
    if (rearm) {
        rc = hc_poke_word_v11(tid, bp, brk_word);
        if (rc != 0) return rc;
    }
    return 0;
}

static void hc_cleanup_v11(pid_t pid, uint64_t bp,
                           uint64_t original_word, bool brk_installed) {
    pid_t writer = 0;
    for (size_t i = 0; i < g_threads_count_v11; ++i) {
        struct hc_thread_v11 *thread = &g_threads_v11[i];
        if (!thread->traced || !hc_tid_in_process_v11(pid, thread->tid)) continue;
        if (!thread->stopped && ptrace(PTRACE_INTERRUPT, thread->tid, 0, 0) == 0) {
            int status = 0;
            if (waitpid(thread->tid, &status, __WALL) == thread->tid && WIFSTOPPED(status))
                thread->stopped = true;
        }
        if (writer == 0 && thread->stopped) writer = thread->tid;
    }

    if (brk_installed && writer != 0) {
        int rc = hc_poke_word_v11(writer, bp, original_word);
        char msg[192];
        snprintf(msg, sizeof(msg), "SWBP cleanup restore tid=%d rc=%d", writer, rc);
        log_stamp(msg);
    }

    for (size_t i = 0; i < g_threads_count_v11; ++i) {
        struct hc_thread_v11 *thread = &g_threads_v11[i];
        if (!thread->traced || !thread->stopped ||
            !hc_tid_in_process_v11(pid, thread->tid)) continue;
        (void)ptrace(PTRACE_DETACH, thread->tid, 0, 0);
        thread->traced = false;
        thread->stopped = false;
    }
    hc_reset_threads_v11();
}

static int hc_intercept_preinit_v11(pid_t pid, uint64_t base,
                                    int cell_x, int cell_y) {
    uint64_t bp = base + RUST_RVA_SAVE_DEVICE_PARAM;
    int mem_fd = open_mem(pid);
    if (mem_fd < 0) return -errno;
    bool signature_ok = hc_save_signature_matches_v11(mem_fd, base);
    close(mem_fd);
    if (!signature_ok) {
        log_stamp("ERROR SWBP v11 save_device_param signature mismatch");
        hc_cleanup_v11(pid, bp, 0, false);
        return 41;
    }

    int rc = hc_seize_rest_and_stop_v11(pid);
    if (rc != 0) {
        hc_cleanup_v11(pid, bp, 0, false);
        return 42;
    }
    pid_t writer = hc_any_stopped_v11();
    if (writer == 0) {
        hc_cleanup_v11(pid, bp, 0, false);
        return 43;
    }

    /* Remove the Rust 5-column clamp before the first Rust grid instruction. */
    rc = hc_patch_return_w0_v11(pid, writer, base + RUST_RVA_CELL_X_MIN,
                                k_x_min_expected, 3, "LauncherCellCount.xMin");
    if (rc != 0) {
        char msg[192];
        snprintf(msg, sizeof(msg), "ERROR EARLY xMin patch rc=%d", rc);
        log_stamp(msg);
    }
    rc = hc_patch_return_w0_v11(pid, writer, base + RUST_RVA_CELL_X_MAX,
                                k_x_max_expected, 9, "LauncherCellCount.xMax");
    if (rc != 0) {
        char msg[192];
        snprintf(msg, sizeof(msg), "ERROR EARLY xMax patch rc=%d", rc);
        log_stamp(msg);
    }

    uint64_t original_word = 0;
    rc = hc_read_word_v11(pid, bp, &original_word);
    if (rc != 0) {
        hc_cleanup_v11(pid, bp, 0, false);
        return 44;
    }
    uint64_t brk_word = (original_word & 0xffffffff00000000ULL) | (uint64_t)A64_BRK_0;
    rc = hc_poke_word_v11(writer, bp, brk_word);
    if (rc != 0) {
        char msg[192];
        snprintf(msg, sizeof(msg), "ERROR SWBP v11 install addr=0x%llx tid=%d rc=%d",
                 (unsigned long long)bp, writer, rc);
        log_stamp(msg);
        hc_cleanup_v11(pid, bp, original_word, false);
        return 45;
    }

    char msg[256];
    snprintf(msg, sizeof(msg),
             "SWBP v11 installed pre-init pid=%d threads=%zu addr=0x%llx original=0x%016llx",
             pid, g_threads_count_v11, (unsigned long long)bp,
             (unsigned long long)original_word);
    log_stamp(msg);

    bool brk_installed = true;
    hc_continue_all_v11();

    int hits = 0;
    bool saw_grid_source = false;
    bool saw_final_loader = false;
    uint64_t deadline = monotonic_ms() + TRACE_WINDOW_MS;

    while (process_exists(pid) && monotonic_ms() < deadline && !saw_final_loader) {
        int status = 0;
        pid_t tid = waitpid(-1, &status, __WALL | WNOHANG);
        if (tid == 0) {
            usleep(100);
            continue;
        }
        if (tid < 0) {
            if (errno == EINTR) continue;
            if (errno == ECHILD) break;
            break;
        }
        if (!hc_tid_in_process_v11(pid, tid)) continue;

        struct hc_thread_v11 *thread = hc_find_thread_v11(tid);
        if (thread == NULL) thread = hc_add_thread_v11(tid, true, WIFSTOPPED(status));
        if (thread == NULL) continue;
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            thread->traced = false;
            continue;
        }
        if (!WIFSTOPPED(status)) continue;
        thread->stopped = true;

        unsigned event = (unsigned)status >> 16;
        int sig = WSTOPSIG(status);
        if (event == PTRACE_EVENT_CLONE) {
            unsigned long new_tid = 0;
            if (ptrace(PTRACE_GETEVENTMSG, tid, 0, &new_tid) == 0)
                hc_add_thread_v11((pid_t)new_tid, true, true);
            if (ptrace(PTRACE_CONT, tid, 0, 0) == 0) thread->stopped = false;
            continue;
        }

        if (sig == SIGTRAP) {
            struct hc_pt_regs_v11 regs;
            if (hc_get_regs_v11(tid, &regs) == 0 && regs.pc == bp) {
                int old_x = 0, old_y = 0, min_x = 0, min_y = 0;
                bool grid_source = false;
                int mrc = hc_mutate_param_v11(pid, tid, base, bp, cell_x, cell_y,
                                               &old_x, &old_y, &min_x, &min_y,
                                               &grid_source);
                if (mrc == 0) {
                    ++hits;
                    saw_grid_source = saw_grid_source || grid_source;
                    bool final_loader = (min_x == 4 && min_y == -1);
                    bool rearm = !final_loader;
                    rc = hc_step_original_v11(tid, bp, original_word, brk_word, rearm);
                    brk_installed = rearm;
                    if (rc != 0) {
                        snprintf(msg, sizeof(msg), "ERROR SWBP v11 single-step/rearm tid=%d rc=%d",
                                 tid, rc);
                        log_stamp(msg);
                        break;
                    }
                    saw_final_loader = final_loader;
                    snprintf(msg, sizeof(msg),
                             "SWBP v11 consumed tid=%d hit=%d grid_source=%s final_loader=%s rearm=%s",
                             tid, hits, grid_source ? "true" : "false",
                             final_loader ? "true" : "false", rearm ? "true" : "false");
                    log_stamp(msg);
                    if (ptrace(PTRACE_CONT, tid, 0, 0) == 0) thread->stopped = false;
                    continue;
                }
            }
        }

        int deliver = (sig == SIGSTOP || sig == SIGTRAP) ? 0 : sig;
        if (ptrace(PTRACE_CONT, tid, 0, (void *)(uintptr_t)deliver) == 0)
            thread->stopped = false;
    }

    snprintf(msg, sizeof(msg),
             "SWBP v11 window done pid=%d hits=%d grid_source=%s final_loader=%s",
             pid, hits, saw_grid_source ? "true" : "false",
             saw_final_loader ? "true" : "false");
    log_stamp(msg);
    hc_cleanup_v11(pid, bp, original_word, brk_installed);
    return saw_grid_source ? 0 : (hits > 0 ? 46 : 40);
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

    char msg[256];
    snprintf(msg, sizeof(msg),
             "native SWBP v11 watcher ready: grid=%s cell=%dx%d pid=%d; pre-init syscall gate",
             grid ? "true" : "false", cell_x, cell_y, getpid());
    log_stamp(msg);
    mark_ready();

    pid_t tracked = 0;
    bool attempted = false;
    unsigned int scan_counter = 0;

    for (;;) {
        bool deep_scan = (scan_counter++ % 4U) == 0U;
        pid_t pid = find_launcher_pid(deep_scan);
        if (pid <= 0) {
            if (tracked != 0) {
                snprintf(msg, sizeof(msg), "launcher pid=%d exited; SWBP v11 re-armed", tracked);
                log_stamp(msg);
                tracked = 0;
                attempted = false;
                hc_reset_threads_v11();
            }
            usleep(500);
            continue;
        }

        if (pid != tracked) {
            tracked = pid;
            attempted = !grid;
            snprintf(msg, sizeof(msg), "launcher candidate detected pid=%d; entering early gate", pid);
            log_stamp(msg);
        }

        if (grid && !attempted) {
            uint64_t base = 0;
            int rc = hc_gate_until_rust_map_v11(pid, &base);
            if (rc == 0) {
                rc = hc_intercept_preinit_v11(pid, base, cell_x, cell_y);
                snprintf(msg, sizeof(msg),
                         "SWBP v11 DeviceParam interception finished pid=%d rc=%d", pid, rc);
                log_stamp(msg);
                attempted = true;
                continue;
            }

            snprintf(msg, sizeof(msg), "ERROR EARLY syscall gate pid=%d rc=%d", pid, rc);
            log_stamp(msg);
            hc_cleanup_v11(pid, 0, 0, false);
            attempted = true;
        }
        usleep(attempted ? 100000 : 250);
    }
}
