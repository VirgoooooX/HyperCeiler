#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <jni.h>
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

#ifndef PTRACE_GETREGSET
#define PTRACE_GETREGSET 0x4204
#endif
#ifndef PTRACE_SETREGSET
#define PTRACE_SETREGSET 0x4205
#endif
#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
#endif
#ifndef PTRACE_INTERRUPT
#define PTRACE_INTERRUPT 0x4207
#endif
#ifndef PTRACE_GETEVENTMSG
#define PTRACE_GETEVENTMSG 0x4201
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
#ifndef NT_ARM_HW_BREAK
#define NT_ARM_HW_BREAK 0x402
#endif
#ifndef __WALL
#define __WALL 0x40000000
#endif

/* Reuse the proven PID/maps/logging helpers from v8, but keep its JNI entry unused. */
#define Java_com_sevtinge_hyperceiler_utils_os4_Os4LauncherRootPatcherCli_nativeWatch \
    Java_com_sevtinge_hyperceiler_utils_os4_Os4LauncherRootPatcherCli_nativeDataProbeUnused
#include "os4_root_data_probe.c"
#undef Java_com_sevtinge_hyperceiler_utils_os4_Os4LauncherRootPatcherCli_nativeWatch

#define RUST_RVA_SAVE_DEVICE_PARAM 0x00785DCCULL
#define TRACE_WINDOW_MS 2500ULL
#define MAX_TRACED_THREADS 256
/* len=4 bytes (0x0f), execute, EL0, enabled. */
#define HWBP_EXEC_EL0_LEN4 ((0x0fU << 5) | (2U << 1) | 1U)

static const uint8_t k_save_signature[16] = {
    0xff, 0x03, 0x03, 0xd1,
    0xfd, 0x7b, 0x0b, 0xa9,
    0xfd, 0xc3, 0x02, 0x91,
    0x88, 0x5d, 0x00, 0xd0,
};

struct hc_pt_regs {
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
};

struct hc_hwdebug_state {
    uint32_t dbg_info;
    uint32_t pad;
    struct {
        uint64_t addr;
        uint32_t ctrl;
        uint32_t pad;
    } dbg_regs[16];
};

struct hc_thread {
    pid_t tid;
    int slot;
    bool traced;
    bool stopped;
    bool armed;
    bool consumed;
};

static struct hc_thread g_threads[MAX_TRACED_THREADS];
static size_t g_threads_count;

static void hc_log(const char *format, long long a, long long b, long long c,
                   long long d, long long e, long long f) {
    char message[512];
    snprintf(message, sizeof(message), format, a, b, c, d, e, f);
    log_stamp(message);
}

static struct hc_thread *hc_find_thread(pid_t tid) {
    for (size_t i = 0; i < g_threads_count; ++i) {
        if (g_threads[i].tid == tid) return &g_threads[i];
    }
    return NULL;
}

static struct hc_thread *hc_add_thread(pid_t tid, bool traced, bool stopped) {
    struct hc_thread *thread = hc_find_thread(tid);
    if (thread != NULL) {
        thread->traced = thread->traced || traced;
        thread->stopped = stopped;
        return thread;
    }
    if (g_threads_count >= MAX_TRACED_THREADS) return NULL;
    thread = &g_threads[g_threads_count++];
    memset(thread, 0, sizeof(*thread));
    thread->tid = tid;
    thread->slot = -1;
    thread->traced = traced;
    thread->stopped = stopped;
    return thread;
}

static bool hc_tid_in_process(pid_t pid, pid_t tid) {
    char path[96];
    snprintf(path, sizeof(path), "/proc/%d/task/%d", pid, tid);
    return access(path, F_OK) == 0;
}

static bool hc_save_signature_matches(int mem_fd, uint64_t base) {
    uint8_t current[sizeof(k_save_signature)];
    return read_exact(mem_fd, base + RUST_RVA_SAVE_DEVICE_PARAM,
                      current, sizeof(current)) &&
           memcmp(current, k_save_signature, sizeof(current)) == 0;
}

static int hc_arm_hwbp(pid_t tid, uint64_t address, int *slot_out) {
    struct hc_hwdebug_state state;
    memset(&state, 0, sizeof(state));
    struct iovec iov = {&state, sizeof(state)};
    if (ptrace(PTRACE_GETREGSET, tid, (void *)(uintptr_t)NT_ARM_HW_BREAK, &iov) != 0)
        return -errno;

    unsigned slots = state.dbg_info & 0xffU;
    if (slots > 16U) slots = 16U;
    int slot = -1;
    for (unsigned i = 0; i < slots; ++i) {
        if ((state.dbg_regs[i].ctrl & 1U) == 0) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) return -ENOSPC;

    state.dbg_regs[slot].addr = address;
    state.dbg_regs[slot].ctrl = HWBP_EXEC_EL0_LEN4;
    iov.iov_len = sizeof(state);
    if (ptrace(PTRACE_SETREGSET, tid, (void *)(uintptr_t)NT_ARM_HW_BREAK, &iov) != 0)
        return -errno;
    *slot_out = slot;
    return 0;
}

static void hc_disarm_hwbp(pid_t tid, int slot) {
    if (slot < 0 || slot >= 16) return;
    struct hc_hwdebug_state state;
    memset(&state, 0, sizeof(state));
    struct iovec iov = {&state, sizeof(state)};
    if (ptrace(PTRACE_GETREGSET, tid, (void *)(uintptr_t)NT_ARM_HW_BREAK, &iov) != 0)
        return;
    state.dbg_regs[slot].ctrl &= ~1U;
    iov.iov_len = sizeof(state);
    (void)ptrace(PTRACE_SETREGSET, tid, (void *)(uintptr_t)NT_ARM_HW_BREAK, &iov);
}

static int hc_configure_thread(struct hc_thread *thread, uint64_t bp) {
    if (thread == NULL || !thread->traced || !thread->stopped ||
        thread->armed || thread->consumed)
        return 0;
    int slot = -1;
    int rc = hc_arm_hwbp(thread->tid, bp, &slot);
    if (rc == 0) {
        thread->slot = slot;
        thread->armed = true;
        hc_log("HWBP armed tid=%lld slot=%lld addr=0x%llx", thread->tid, slot,
               (long long)bp, 0, 0, 0);
    } else {
        hc_log("ERROR HWBP arm tid=%lld rc=%lld errno=%lld", thread->tid, rc,
               -rc, 0, 0, 0);
    }
    return rc;
}

static int hc_seize_existing(pid_t pid, uint64_t bp) {
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

    for (size_t i = 0; i < count; ++i) {
        pid_t tid = tids[i];
        long options = PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXIT;
        if (ptrace(PTRACE_SEIZE, tid, 0, (void *)(uintptr_t)options) != 0) {
            if (errno != ESRCH)
                hc_log("ERROR PTRACE_SEIZE tid=%lld errno=%lld", tid, errno, 0, 0, 0, 0);
            continue;
        }
        struct hc_thread *thread = hc_add_thread(tid, true, false);
        if (thread == NULL) continue;
        if (ptrace(PTRACE_INTERRUPT, tid, 0, 0) != 0) continue;
        int status = 0;
        if (waitpid(tid, &status, __WALL) != tid || !WIFSTOPPED(status)) continue;
        thread->stopped = true;
        hc_configure_thread(thread, bp);
        if (ptrace(PTRACE_CONT, tid, 0, 0) == 0) thread->stopped = false;
    }
    return 0;
}

static int hc_mutate_x0_param(pid_t pid, pid_t tid, uint64_t bp,
                              int cell_x, int cell_y) {
    struct hc_pt_regs regs;
    memset(&regs, 0, sizeof(regs));
    struct iovec iov = {&regs, sizeof(regs)};
    if (ptrace(PTRACE_GETREGSET, tid, (void *)(uintptr_t)NT_PRSTATUS, &iov) != 0)
        return -errno;
    if (regs.pc != bp) return 1;

    uint64_t param = regs.regs[0];
    int mem_fd = open_mem(pid);
    if (mem_fd < 0) return -errno;
    uint8_t value[16] = {0};
    bool read_ok = read_exact(mem_fd, param, value, sizeof(value));
    int old_x = 0, old_y = 0, min_x = 0, min_y = 0;
    if (read_ok) {
        old_x = read_i32_le(value);
        old_y = read_i32_le(value + 4);
        min_x = read_i32_le(value + 8);
        min_y = read_i32_le(value + 12);
    }
    put_i32_le(value, cell_x);
    put_i32_le(value + 4, cell_y);
    bool ok = write_exact(mem_fd, param, value, 8);
    uint8_t verify[8];
    ok = ok && read_exact(mem_fd, param, verify, sizeof(verify)) &&
         memcmp(verify, value, sizeof(verify)) == 0;
    close(mem_fd);

    char message[512];
    snprintf(message, sizeof(message),
             "HWBP hit tid=%d pc=0x%llx x0=0x%llx DeviceParam %dx%d min=%d,%d -> %dx%d write=%s",
             tid, (unsigned long long)regs.pc, (unsigned long long)param,
             old_x, old_y, min_x, min_y, cell_x, cell_y, ok ? "ok" : "FAILED");
    log_stamp(message);
    return ok ? 0 : -EIO;
}

static void hc_cleanup(pid_t pid) {
    for (size_t i = 0; i < g_threads_count; ++i) {
        struct hc_thread *thread = &g_threads[i];
        if (!thread->traced || !hc_tid_in_process(pid, thread->tid)) continue;
        if (!thread->stopped && ptrace(PTRACE_INTERRUPT, thread->tid, 0, 0) == 0) {
            int status = 0;
            if (waitpid(thread->tid, &status, __WALL) == thread->tid && WIFSTOPPED(status))
                thread->stopped = true;
        }
        if (thread->stopped) {
            if (thread->armed) hc_disarm_hwbp(thread->tid, thread->slot);
            (void)ptrace(PTRACE_DETACH, thread->tid, 0, 0);
        }
    }
    memset(g_threads, 0, sizeof(g_threads));
    g_threads_count = 0;
}

static int hc_intercept(pid_t pid, uint64_t base, int cell_x, int cell_y) {
    g_threads_count = 0;
    uint64_t bp = base + RUST_RVA_SAVE_DEVICE_PARAM;
    int rc = hc_seize_existing(pid, bp);
    if (rc != 0) {
        hc_cleanup(pid);
        return rc;
    }
    hc_log("HWBP interception active pid=%lld threads=%lld addr=0x%llx",
           pid, (long long)g_threads_count, (long long)bp, 0, 0, 0);

    int hits = 0;
    uint64_t deadline = monotonic_ms() + TRACE_WINDOW_MS;
    while (process_exists(pid) && monotonic_ms() < deadline) {
        int status = 0;
        pid_t tid = waitpid(-1, &status, __WALL | WNOHANG);
        if (tid == 0) {
            usleep(200);
            continue;
        }
        if (tid < 0) {
            if (errno == EINTR) continue;
            if (errno == ECHILD) break;
            break;
        }
        if (!hc_tid_in_process(pid, tid)) continue;

        struct hc_thread *thread = hc_find_thread(tid);
        if (thread == NULL) thread = hc_add_thread(tid, true, WIFSTOPPED(status));
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
            if (ptrace(PTRACE_GETEVENTMSG, tid, 0, &new_tid) == 0) {
                hc_add_thread((pid_t)new_tid, true, true);
                hc_log("PTRACE clone parent=%lld new_tid=%lld", tid, (long long)new_tid, 0, 0, 0, 0);
            }
            if (ptrace(PTRACE_CONT, tid, 0, 0) == 0) thread->stopped = false;
            continue;
        }

        if (!thread->armed) {
            if (!thread->consumed) hc_configure_thread(thread, bp);
            if (ptrace(PTRACE_CONT, tid, 0, 0) == 0) thread->stopped = false;
            continue;
        }

        bool handled = false;
        if (sig == SIGTRAP && hc_mutate_x0_param(pid, tid, bp, cell_x, cell_y) == 0) {
            ++hits;
            handled = true;
            hc_disarm_hwbp(tid, thread->slot);
            thread->armed = false;
            thread->consumed = true;
            hc_log("HWBP consumed tid=%lld total_hits=%lld", tid, hits, 0, 0, 0, 0);
        }

        int deliver = (handled || sig == SIGTRAP || sig == SIGSTOP) ? 0 : sig;
        if (ptrace(PTRACE_CONT, tid, 0, (void *)(uintptr_t)deliver) == 0)
            thread->stopped = false;
    }

    hc_log("HWBP interception window done pid=%lld hits=%lld", pid, hits, 0, 0, 0, 0);
    hc_cleanup(pid);
    return hits > 0 ? 0 : 40;
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
    (void)env; (void)clazz; (void)hotseat_value; (void)icon_value; (void)icon_size_value;
    setvbuf(stdout, NULL, _IOLBF, 0);
    const bool grid = grid_value == JNI_TRUE;
    const int cell_x = (int)cell_x_value;
    const int cell_y = (int)cell_y_value;

    char message[256];
    snprintf(message, sizeof(message),
             "native HWBP watcher ready: grid=%s cell=%dx%d pid=%d; instruction patches disabled",
             grid ? "true" : "false", cell_x, cell_y, getpid());
    log_stamp(message);
    mark_ready();

    pid_t tracked = 0;
    bool done = false;
    for (;;) {
        pid_t pid = find_launcher_pid(true);
        if (pid <= 0) {
            if (tracked != 0) {
                snprintf(message, sizeof(message), "launcher pid=%d exited; HWBP watcher re-armed", tracked);
                log_stamp(message);
                tracked = 0;
                done = false;
            }
            usleep(1000);
            continue;
        }
        if (pid != tracked) {
            tracked = pid;
            done = !grid;
            snprintf(message, sizeof(message), "launcher candidate detected pid=%d", pid);
            log_stamp(message);
        }
        if (grid && !done) {
            int mem_fd = open_mem(pid);
            if (mem_fd >= 0) {
                uint64_t base = 0;
                if (find_rust_base(pid, mem_fd, &base) && hc_save_signature_matches(mem_fd, base)) {
                    close(mem_fd);
                    snprintf(message, sizeof(message), "Rust HWBP target mapped pid=%d base=0x%llx",
                             pid, (unsigned long long)base);
                    log_stamp(message);
                    int intercept_rc = hc_intercept(pid, base, cell_x, cell_y);
                    snprintf(message, sizeof(message),
                             "HWBP DeviceParam interception finished pid=%d rc=%d", pid, intercept_rc);
                    log_stamp(message);
                    done = true;
                } else {
                    close(mem_fd);
                }
            }
        }
        usleep(done ? 100000 : 500);
    }
}
