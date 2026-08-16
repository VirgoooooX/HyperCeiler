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
#ifndef PTRACE_O_TRACEFORK
#define PTRACE_O_TRACEFORK 0x00000002
#endif
#ifndef PTRACE_O_TRACEVFORK
#define PTRACE_O_TRACEVFORK 0x00000004
#endif
#ifndef PTRACE_O_TRACECLONE
#define PTRACE_O_TRACECLONE 0x00000008
#endif
#ifndef PTRACE_O_TRACEEXIT
#define PTRACE_O_TRACEEXIT 0x00000040
#endif
#ifndef PTRACE_EVENT_FORK
#define PTRACE_EVENT_FORK 1
#endif
#ifndef PTRACE_EVENT_VFORK
#define PTRACE_EVENT_VFORK 2
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
    Java_com_sevtinge_hyperceiler_utils_os4_Os4LauncherRootPatcherCli_nativeDataProbeUnusedV12
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

#define SPAWNER_ARM_TIMEOUT_MS 3000ULL
#define CHILD_MAP_TIMEOUT_MS   3500ULL
#define TRACE_WINDOW_MS        2500ULL
#define MAX_TRACED_THREADS     256
#define A64_BRK_0              0xD4200000U

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

struct hc_pt_regs_v12 {
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
};

struct hc_thread_v12 {
    pid_t tid;
    bool traced;
    bool stopped;
};

static struct hc_thread_v12 g_threads_v12[MAX_TRACED_THREADS];
static size_t g_threads_count_v12;

static struct hc_thread_v12 *hc_find_thread_v12(pid_t tid) {
    for (size_t i = 0; i < g_threads_count_v12; ++i) {
        if (g_threads_v12[i].tid == tid) return &g_threads_v12[i];
    }
    return NULL;
}

static struct hc_thread_v12 *hc_add_thread_v12(pid_t tid, bool traced, bool stopped) {
    struct hc_thread_v12 *thread = hc_find_thread_v12(tid);
    if (thread != NULL) {
        thread->traced = thread->traced || traced;
        thread->stopped = stopped;
        return thread;
    }
    if (g_threads_count_v12 >= MAX_TRACED_THREADS) return NULL;
    thread = &g_threads_v12[g_threads_count_v12++];
    memset(thread, 0, sizeof(*thread));
    thread->tid = tid;
    thread->traced = traced;
    thread->stopped = stopped;
    return thread;
}

static void hc_reset_threads_v12(void) {
    memset(g_threads_v12, 0, sizeof(g_threads_v12));
    g_threads_count_v12 = 0;
}

static bool hc_tid_in_process_v12(pid_t pid, pid_t tid) {
    char path[96];
    snprintf(path, sizeof(path), "/proc/%d/task/%d", pid, tid);
    return access(path, F_OK) == 0;
}

static bool hc_comm_equals_v12(pid_t pid, const char *expected) {
    char path[64];
    char comm[128] = {0};
    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    FILE *f = fopen(path, "re");
    if (f == NULL) return false;
    bool match = false;
    if (fgets(comm, sizeof(comm), f) != NULL) {
        char *nl = strchr(comm, '\n');
        if (nl != NULL) *nl = '\0';
        match = strcmp(comm, expected) == 0;
    }
    fclose(f);
    return match;
}

static pid_t hc_find_spawner_v12(void) {
    DIR *dir = opendir("/proc");
    if (dir == NULL) return 0;
    pid_t found = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!numeric_name(entry->d_name)) continue;
        pid_t pid = (pid_t)strtol(entry->d_name, NULL, 10);
        if (hc_comm_equals_v12(pid, "hyos_spawner")) {
            found = pid;
            break;
        }
    }
    closedir(dir);
    return found;
}

static pid_t hc_read_tgid_v12(pid_t pid) {
    char path[64];
    char line[256];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *f = fopen(path, "re");
    if (f == NULL) return 0;
    pid_t tgid = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "Tgid:", 5) == 0) {
            tgid = (pid_t)strtol(line + 5, NULL, 10);
            break;
        }
    }
    fclose(f);
    return tgid;
}

static int hc_get_regs_v12(pid_t tid, struct hc_pt_regs_v12 *regs) {
    memset(regs, 0, sizeof(*regs));
    struct iovec iov = {regs, sizeof(*regs)};
    if (ptrace(PTRACE_GETREGSET, tid, (void *)(uintptr_t)NT_PRSTATUS, &iov) != 0)
        return -errno;
    return 0;
}

static int hc_poke_word_v12(pid_t tid, uint64_t address, uint64_t value) {
    errno = 0;
    if (ptrace(PTRACE_POKETEXT, tid, (void *)(uintptr_t)address,
               (void *)(uintptr_t)value) != 0)
        return -errno;
    return 0;
}

static int hc_read_word_v12(pid_t pid, uint64_t address, uint64_t *word_out) {
    int mem_fd = open_mem(pid);
    if (mem_fd < 0) return -errno;
    bool ok = read_exact(mem_fd, address, word_out, sizeof(*word_out));
    int err = errno;
    close(mem_fd);
    return ok ? 0 : -err;
}

static uint64_t hc_return_w0_word_v12(int value) {
    uint32_t movz = 0x52800000U | (((uint32_t)value & 0xffffU) << 5);
    return (uint64_t)movz | ((uint64_t)0xd65f03c0U << 32);
}

static int hc_patch_return_w0_v12(pid_t pid, pid_t writer, uint64_t address,
                                  const uint8_t expected[8], int value,
                                  const char *name) {
    uint64_t current = 0;
    int rc = hc_read_word_v12(pid, address, &current);
    if (rc != 0) return rc;

    uint64_t expected_word = 0;
    memcpy(&expected_word, expected, sizeof(expected_word));
    uint64_t desired = hc_return_w0_word_v12(value);
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

    rc = hc_poke_word_v12(writer, address, desired);
    if (rc != 0) return rc;
    uint64_t verify = 0;
    rc = hc_read_word_v12(pid, address, &verify);
    if (rc != 0 || verify != desired) return -EIO;

    char msg[192];
    snprintf(msg, sizeof(msg), "EARLY %s patched addr=0x%llx value=%d",
             name, (unsigned long long)address, value);
    log_stamp(msg);
    return 0;
}

static int hc_wait_stopped_v12(pid_t tid, uint64_t timeout_ms, int *status_out) {
    uint64_t deadline = monotonic_ms() + timeout_ms;
    for (;;) {
        int status = 0;
        pid_t got = waitpid(tid, &status, __WALL | WNOHANG);
        if (got == tid) {
            if (status_out != NULL) *status_out = status;
            return WIFSTOPPED(status) ? 0 : -ESRCH;
        }
        if (got < 0 && errno != EINTR) return -errno;
        if (monotonic_ms() >= deadline) return -ETIMEDOUT;
        usleep(100);
    }
}

static int hc_arm_spawner_v12(pid_t spawner) {
    long options = PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK |
                   PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXIT;
    if (ptrace(PTRACE_SEIZE, spawner, 0, (void *)(uintptr_t)options) != 0)
        return -errno;
    if (ptrace(PTRACE_INTERRUPT, spawner, 0, 0) != 0) {
        int err = -errno;
        (void)ptrace(PTRACE_DETACH, spawner, 0, 0);
        return err;
    }
    int status = 0;
    int rc = hc_wait_stopped_v12(spawner, 1000, &status);
    if (rc != 0) {
        (void)ptrace(PTRACE_DETACH, spawner, 0, 0);
        return rc;
    }
    if (ptrace(PTRACE_CONT, spawner, 0, 0) != 0) {
        int err = -errno;
        (void)ptrace(PTRACE_DETACH, spawner, 0, 0);
        return err;
    }
    return 0;
}

static void hc_detach_if_traced_v12(pid_t tid) {
    if (tid <= 0 || !process_exists(tid)) return;
    if (ptrace(PTRACE_INTERRUPT, tid, 0, 0) == 0) {
        int status = 0;
        (void)hc_wait_stopped_v12(tid, 300, &status);
    }
    (void)ptrace(PTRACE_DETACH, tid, 0, 0);
}

/*
 * Wait on the already-armed hyos_spawner. PTRACE_EVENT_FORK fires while both
 * sides of fork are stopped. The new Rust process is therefore under ptrace
 * before it can execute its first post-fork user-space instruction.
 */
static int hc_wait_for_process_child_v12(pid_t spawner, pid_t *child_out,
                                         unsigned *event_out) {
    uint64_t deadline = monotonic_ms() + SPAWNER_ARM_TIMEOUT_MS;
    while (process_exists(spawner) && monotonic_ms() < deadline) {
        int status = 0;
        pid_t got = waitpid(spawner, &status, __WALL | WNOHANG);
        if (got == 0) {
            usleep(100);
            continue;
        }
        if (got < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) return -ESRCH;
        if (!WIFSTOPPED(status)) continue;

        unsigned event = (unsigned)status >> 16;
        int sig = WSTOPSIG(status);
        if (event == PTRACE_EVENT_FORK || event == PTRACE_EVENT_VFORK ||
            event == PTRACE_EVENT_CLONE) {
            unsigned long new_pid_ul = 0;
            if (ptrace(PTRACE_GETEVENTMSG, spawner, 0, &new_pid_ul) != 0) {
                int err = -errno;
                (void)ptrace(PTRACE_CONT, spawner, 0, 0);
                return err;
            }
            pid_t child = (pid_t)new_pid_ul;
            int child_status = 0;
            int rc = hc_wait_stopped_v12(child, 1000, &child_status);
            if (rc != 0) {
                (void)ptrace(PTRACE_CONT, spawner, 0, 0);
                continue;
            }

            pid_t tgid = hc_read_tgid_v12(child);
            bool process_child = tgid == child || event != PTRACE_EVENT_CLONE;
            if (!process_child) {
                (void)ptrace(PTRACE_DETACH, child, 0, 0);
                (void)ptrace(PTRACE_CONT, spawner, 0, 0);
                continue;
            }

            if (ptrace(PTRACE_DETACH, spawner, 0, 0) != 0) {
                (void)ptrace(PTRACE_CONT, spawner, 0, 0);
            }
            *child_out = child;
            *event_out = event;
            hc_reset_threads_v12();
            hc_add_thread_v12(child, true, true);
            return 0;
        }

        int deliver = (sig == SIGSTOP || sig == SIGTRAP) ? 0 : sig;
        if (ptrace(PTRACE_CONT, spawner, 0, (void *)(uintptr_t)deliver) != 0)
            return -errno;
    }
    return -ETIMEDOUT;
}

/*
 * The child is already stopped because it came from a ptrace fork event.
 * Advance it one syscall-stop at a time and inspect mappings after each stop.
 * Once the exact libapp_launcher.so build becomes visible, we are still at the
 * mmap stop, before Rust user code can consume the default 5x8 DeviceParam.
 */
static int hc_gate_traced_child_until_rust_map_v12(pid_t pid, uint64_t *base_out,
                                                    int *stops_out) {
    long options = PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXIT;
    if (ptrace(PTRACE_SETOPTIONS, pid, 0, (void *)(uintptr_t)options) != 0)
        return -errno;

    uint64_t start = monotonic_ms();
    int stops = 0;
    int pending_signal = 0;
    while (process_exists(pid) && monotonic_ms() - start < CHILD_MAP_TIMEOUT_MS) {
        int mem_fd = open_mem(pid);
        if (mem_fd >= 0) {
            uint64_t base = 0;
            bool mapped = find_rust_base(pid, mem_fd, &base);
            close(mem_fd);
            if (mapped) {
                *base_out = base;
                if (stops_out != NULL) *stops_out = stops;
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "PRE-FORK rust map caught child=%d base=0x%llx syscall_stops=%d elapsed=%llums",
                         pid, (unsigned long long)base, stops,
                         (unsigned long long)(monotonic_ms() - start));
                log_stamp(msg);
                return 0;
            }
        }

        if (ptrace(PTRACE_SYSCALL, pid, 0, (void *)(uintptr_t)pending_signal) != 0)
            return -errno;
        struct hc_thread_v12 *leader = hc_find_thread_v12(pid);
        if (leader != NULL) leader->stopped = false;
        pending_signal = 0;

        int status = 0;
        if (waitpid(pid, &status, __WALL) != pid) {
            if (errno == EINTR) continue;
            return -errno;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) return -ESRCH;
        if (!WIFSTOPPED(status)) continue;
        if (leader != NULL) leader->stopped = true;
        ++stops;

        int sig = WSTOPSIG(status);
        if (sig != (SIGTRAP | 0x80) && sig != SIGTRAP && sig != SIGSTOP)
            pending_signal = sig;
    }
    if (stops_out != NULL) *stops_out = stops;
    return -ETIMEDOUT;
}

static int hc_seize_rest_and_stop_v12(pid_t pid) {
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
        struct hc_thread_v12 *known = hc_find_thread_v12(tid);
        if (known != NULL && known->traced) {
            (void)ptrace(PTRACE_SETOPTIONS, tid, 0, (void *)(uintptr_t)options);
            continue;
        }
        if (ptrace(PTRACE_SEIZE, tid, 0, (void *)(uintptr_t)options) != 0) {
            if (errno != ESRCH) {
                char msg[192];
                snprintf(msg, sizeof(msg), "ERROR PRE-FORK PTRACE_SEIZE tid=%d errno=%d (%s)",
                         tid, errno, strerror(errno));
                log_stamp(msg);
            }
            continue;
        }
        hc_add_thread_v12(tid, true, false);
    }

    for (size_t i = 0; i < g_threads_count_v12; ++i) {
        struct hc_thread_v12 *thread = &g_threads_v12[i];
        if (!thread->traced || thread->stopped) continue;
        if (ptrace(PTRACE_INTERRUPT, thread->tid, 0, 0) != 0) continue;
        int status = 0;
        if (waitpid(thread->tid, &status, __WALL) == thread->tid && WIFSTOPPED(status))
            thread->stopped = true;
    }
    return g_threads_count_v12 > 0 ? 0 : -ESRCH;
}

static pid_t hc_any_stopped_v12(void) {
    for (size_t i = 0; i < g_threads_count_v12; ++i) {
        if (g_threads_v12[i].traced && g_threads_v12[i].stopped)
            return g_threads_v12[i].tid;
    }
    return 0;
}

static void hc_continue_all_v12(void) {
    for (size_t i = 0; i < g_threads_count_v12; ++i) {
        struct hc_thread_v12 *thread = &g_threads_v12[i];
        if (!thread->traced || !thread->stopped) continue;
        if (ptrace(PTRACE_CONT, thread->tid, 0, 0) == 0)
            thread->stopped = false;
    }
}

static bool hc_save_signature_matches_v12(int mem_fd, uint64_t base) {
    uint8_t current[sizeof(k_save_signature)];
    return read_exact(mem_fd, base + RUST_RVA_SAVE_DEVICE_PARAM,
                      current, sizeof(current)) &&
           memcmp(current, k_save_signature, sizeof(current)) == 0;
}

static bool hc_write_i32_v12(int mem_fd, uint64_t address, int value) {
    uint8_t bytes[4];
    put_i32_le(bytes, value);
    if (!write_exact(mem_fd, address, bytes, sizeof(bytes))) return false;
    uint8_t verify[4];
    return read_exact(mem_fd, address, verify, sizeof(verify)) &&
           memcmp(bytes, verify, sizeof(bytes)) == 0;
}

static int hc_mutate_param_v12(pid_t pid, pid_t tid, uint64_t base, uint64_t bp,
                               int cell_x, int cell_y,
                               int *old_x_out, int *old_y_out,
                               int *min_x_out, int *min_y_out,
                               bool *grid_source_out) {
    struct hc_pt_regs_v12 regs;
    int rc = hc_get_regs_v12(tid, &regs);
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
        source_ok = hc_write_i32_v12(mem_fd, source + GRID_SOURCE_X_ACTIVE, cell_x) &&
                    hc_write_i32_v12(mem_fd, source + GRID_SOURCE_X_DEFAULT, cell_x) &&
                    hc_write_i32_v12(mem_fd, source + GRID_SOURCE_Y, cell_y);
    }
    close(mem_fd);

    if (old_x_out) *old_x_out = old_x;
    if (old_y_out) *old_y_out = old_y;
    if (min_x_out) *min_x_out = min_x;
    if (min_y_out) *min_y_out = min_y;
    if (grid_source_out) *grid_source_out = grid_source;

    char msg[640];
    snprintf(msg, sizeof(msg),
             "SWBP v12 hit tid=%d pc=0x%llx lr=0x%llx caller_rva=0x%llx x0=0x%llx x19=0x%llx DeviceParam %dx%d min=%d,%d -> %dx%d write=%s grid_source=%s source_write=%s",
             tid, (unsigned long long)regs.pc, (unsigned long long)lr,
             (unsigned long long)caller_rva, (unsigned long long)param,
             (unsigned long long)source, old_x, old_y, min_x, min_y,
             cell_x, cell_y, ok ? "ok" : "FAILED",
             grid_source ? "true" : "false", source_ok ? "ok" : "FAILED");
    log_stamp(msg);
    return ok && source_ok ? 0 : -EIO;
}

static int hc_step_original_v12(pid_t tid, uint64_t bp,
                                uint64_t original_word, uint64_t brk_word,
                                bool rearm) {
    int rc = hc_poke_word_v12(tid, bp, original_word);
    if (rc != 0) return rc;
    if (ptrace(PTRACE_SINGLESTEP, tid, 0, 0) != 0) return -errno;
    int status = 0;
    if (waitpid(tid, &status, __WALL) != tid || !WIFSTOPPED(status)) return -EIO;
    struct hc_thread_v12 *thread = hc_find_thread_v12(tid);
    if (thread != NULL) thread->stopped = true;
    if (rearm) {
        rc = hc_poke_word_v12(tid, bp, brk_word);
        if (rc != 0) return rc;
    }
    return 0;
}

static void hc_cleanup_v12(pid_t pid, uint64_t bp,
                           uint64_t original_word, bool brk_installed) {
    pid_t writer = 0;
    for (size_t i = 0; i < g_threads_count_v12; ++i) {
        struct hc_thread_v12 *thread = &g_threads_v12[i];
        if (!thread->traced || !hc_tid_in_process_v12(pid, thread->tid)) continue;
        if (!thread->stopped && ptrace(PTRACE_INTERRUPT, thread->tid, 0, 0) == 0) {
            int status = 0;
            if (waitpid(thread->tid, &status, __WALL) == thread->tid && WIFSTOPPED(status))
                thread->stopped = true;
        }
        if (writer == 0 && thread->stopped) writer = thread->tid;
    }

    if (brk_installed && writer != 0) {
        int rc = hc_poke_word_v12(writer, bp, original_word);
        char msg[192];
        snprintf(msg, sizeof(msg), "SWBP v12 cleanup restore tid=%d rc=%d", writer, rc);
        log_stamp(msg);
    }

    for (size_t i = 0; i < g_threads_count_v12; ++i) {
        struct hc_thread_v12 *thread = &g_threads_v12[i];
        if (!thread->traced || !thread->stopped ||
            !hc_tid_in_process_v12(pid, thread->tid)) continue;
        (void)ptrace(PTRACE_DETACH, thread->tid, 0, 0);
        thread->traced = false;
        thread->stopped = false;
    }
    hc_reset_threads_v12();
}

static int hc_intercept_preinit_v12(pid_t pid, uint64_t base,
                                    int cell_x, int cell_y) {
    uint64_t bp = base + RUST_RVA_SAVE_DEVICE_PARAM;
    int mem_fd = open_mem(pid);
    if (mem_fd < 0) return -errno;
    bool signature_ok = hc_save_signature_matches_v12(mem_fd, base);
    close(mem_fd);
    if (!signature_ok) {
        log_stamp("ERROR SWBP v12 save_device_param signature mismatch");
        hc_cleanup_v12(pid, bp, 0, false);
        return 41;
    }

    int rc = hc_seize_rest_and_stop_v12(pid);
    if (rc != 0) {
        hc_cleanup_v12(pid, bp, 0, false);
        return 42;
    }
    pid_t writer = hc_any_stopped_v12();
    if (writer == 0) {
        hc_cleanup_v12(pid, bp, 0, false);
        return 43;
    }

    rc = hc_patch_return_w0_v12(pid, writer, base + RUST_RVA_CELL_X_MIN,
                                k_x_min_expected, 3, "LauncherCellCount.xMin");
    if (rc != 0) {
        char msg[192];
        snprintf(msg, sizeof(msg), "ERROR PRE-FORK xMin patch rc=%d", rc);
        log_stamp(msg);
    }
    rc = hc_patch_return_w0_v12(pid, writer, base + RUST_RVA_CELL_X_MAX,
                                k_x_max_expected, 9, "LauncherCellCount.xMax");
    if (rc != 0) {
        char msg[192];
        snprintf(msg, sizeof(msg), "ERROR PRE-FORK xMax patch rc=%d", rc);
        log_stamp(msg);
    }

    uint64_t original_word = 0;
    rc = hc_read_word_v12(pid, bp, &original_word);
    if (rc != 0) {
        hc_cleanup_v12(pid, bp, 0, false);
        return 44;
    }
    uint64_t brk_word = (original_word & 0xffffffff00000000ULL) | (uint64_t)A64_BRK_0;
    rc = hc_poke_word_v12(writer, bp, brk_word);
    if (rc != 0) {
        char msg[192];
        snprintf(msg, sizeof(msg), "ERROR SWBP v12 install addr=0x%llx tid=%d rc=%d",
                 (unsigned long long)bp, writer, rc);
        log_stamp(msg);
        hc_cleanup_v12(pid, bp, original_word, false);
        return 45;
    }

    char msg[256];
    snprintf(msg, sizeof(msg),
             "SWBP v12 installed before Rust init child=%d threads=%zu addr=0x%llx original=0x%016llx",
             pid, g_threads_count_v12, (unsigned long long)bp,
             (unsigned long long)original_word);
    log_stamp(msg);

    bool brk_installed = true;
    hc_continue_all_v12();

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
        if (!hc_tid_in_process_v12(pid, tid)) continue;

        struct hc_thread_v12 *thread = hc_find_thread_v12(tid);
        if (thread == NULL) thread = hc_add_thread_v12(tid, true, WIFSTOPPED(status));
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
                hc_add_thread_v12((pid_t)new_tid, true, true);
            if (ptrace(PTRACE_CONT, tid, 0, 0) == 0) thread->stopped = false;
            continue;
        }

        if (sig == SIGTRAP) {
            struct hc_pt_regs_v12 regs;
            if (hc_get_regs_v12(tid, &regs) == 0 && regs.pc == bp) {
                int old_x = 0, old_y = 0, min_x = 0, min_y = 0;
                bool grid_source = false;
                int mrc = hc_mutate_param_v12(pid, tid, base, bp, cell_x, cell_y,
                                               &old_x, &old_y, &min_x, &min_y,
                                               &grid_source);
                if (mrc == 0) {
                    ++hits;
                    saw_grid_source = saw_grid_source || grid_source;
                    bool final_loader = (min_x == 4 && min_y == -1);
                    bool rearm = !final_loader;
                    rc = hc_step_original_v12(tid, bp, original_word, brk_word, rearm);
                    brk_installed = rearm;
                    if (rc != 0) {
                        snprintf(msg, sizeof(msg), "ERROR SWBP v12 single-step/rearm tid=%d rc=%d",
                                 tid, rc);
                        log_stamp(msg);
                        break;
                    }
                    saw_final_loader = final_loader;
                    snprintf(msg, sizeof(msg),
                             "SWBP v12 consumed tid=%d hit=%d grid_source=%s final_loader=%s rearm=%s",
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
             "SWBP v12 window done child=%d hits=%d grid_source=%s final_loader=%s",
             pid, hits, saw_grid_source ? "true" : "false",
             saw_final_loader ? "true" : "false");
    log_stamp(msg);
    hc_cleanup_v12(pid, bp, original_word, brk_installed);
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

    char msg[320];
    snprintf(msg, sizeof(msg),
             "native SWBP v12 watcher ready: grid=%s cell=%dx%d pid=%d; arming hyos_spawner pre-fork gate",
             grid ? "true" : "false", cell_x, cell_y, getpid());
    log_stamp(msg);

    if (!grid) {
        mark_ready();
        for (;;) sleep(3600);
    }

    bool ready_marked = false;
    for (;;) {
        pid_t spawner = 0;
        uint64_t scan_start = monotonic_ms();
        while (spawner <= 0 && monotonic_ms() - scan_start < SPAWNER_ARM_TIMEOUT_MS) {
            spawner = hc_find_spawner_v12();
            if (spawner <= 0) usleep(1000);
        }
        if (spawner <= 0) {
            log_stamp("ERROR SWBP v12 hyos_spawner not found; retrying without killing launcher");
            if (!ready_marked) {
                mark_ready();
                ready_marked = true;
            }
            usleep(250000);
            continue;
        }

        int rc = hc_arm_spawner_v12(spawner);
        if (rc != 0) {
            snprintf(msg, sizeof(msg), "ERROR SWBP v12 cannot arm hyos_spawner pid=%d rc=%d", spawner, rc);
            log_stamp(msg);
            if (!ready_marked) {
                mark_ready();
                ready_marked = true;
            }
            usleep(250000);
            continue;
        }

        snprintf(msg, sizeof(msg), "PRE-FORK gate armed on hyos_spawner pid=%d", spawner);
        log_stamp(msg);
        if (!ready_marked) {
            mark_ready();
            ready_marked = true;
        }

        pid_t child = 0;
        unsigned event = 0;
        rc = hc_wait_for_process_child_v12(spawner, &child, &event);
        if (rc != 0) {
            snprintf(msg, sizeof(msg), "ERROR PRE-FORK wait child spawner=%d rc=%d; re-arming", spawner, rc);
            log_stamp(msg);
            hc_detach_if_traced_v12(spawner);
            usleep(10000);
            continue;
        }

        snprintf(msg, sizeof(msg),
                 "PRE-FORK child caught pid=%d event=%u tgid=%d; parent spawner released",
                 child, event, hc_read_tgid_v12(child));
        log_stamp(msg);

        uint64_t base = 0;
        int stops = 0;
        rc = hc_gate_traced_child_until_rust_map_v12(child, &base, &stops);
        if (rc != 0) {
            snprintf(msg, sizeof(msg),
                     "PRE-FORK child pid=%d did not become supported launcher rc=%d syscall_stops=%d; detach/re-arm",
                     child, rc, stops);
            log_stamp(msg);
            hc_cleanup_v12(child, 0, 0, false);
            usleep(10000);
            continue;
        }

        rc = hc_intercept_preinit_v12(child, base, cell_x, cell_y);
        snprintf(msg, sizeof(msg),
                 "SWBP v12 DeviceParam interception finished child=%d rc=%d", child, rc);
        log_stamp(msg);

        while (process_exists(child)) usleep(100000);
        snprintf(msg, sizeof(msg), "launcher child=%d exited; SWBP v12 re-arming spawner", child);
        log_stamp(msg);
    }
}
