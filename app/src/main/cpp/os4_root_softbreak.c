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
    Java_com_sevtinge_hyperceiler_utils_os4_Os4LauncherRootPatcherCli_nativeDataProbeUnused
#include "os4_root_data_probe.c"
#undef Java_com_sevtinge_hyperceiler_utils_os4_Os4LauncherRootPatcherCli_nativeWatch

#define RUST_RVA_SAVE_DEVICE_PARAM 0x00785DCCULL
#define TRACE_WINDOW_MS 2200ULL
#define MAX_TRACED_THREADS 256
#define A64_BRK_0 0xD4200000U

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

struct hc_thread {
    pid_t tid;
    bool traced;
    bool stopped;
};

static struct hc_thread g_threads[MAX_TRACED_THREADS];
static size_t g_threads_count;

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

static int hc_get_regs(pid_t tid, struct hc_pt_regs *regs) {
    memset(regs, 0, sizeof(*regs));
    struct iovec iov = {regs, sizeof(*regs)};
    if (ptrace(PTRACE_GETREGSET, tid, (void *)(uintptr_t)NT_PRSTATUS, &iov) != 0)
        return -errno;
    return 0;
}

static int hc_poke_word(pid_t tid, uint64_t address, uint64_t value) {
    errno = 0;
    if (ptrace(PTRACE_POKETEXT, tid, (void *)(uintptr_t)address,
               (void *)(uintptr_t)value) != 0) {
        return -errno;
    }
    return 0;
}

static int hc_read_code_word(pid_t pid, uint64_t address, uint64_t *word_out) {
    int mem_fd = open_mem(pid);
    if (mem_fd < 0) return -errno;
    bool ok = read_exact(mem_fd, address, word_out, sizeof(*word_out));
    int err = errno;
    close(mem_fd);
    return ok ? 0 : -err;
}

static int hc_seize_and_stop_all(pid_t pid) {
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

    long options = PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXIT;
    for (size_t i = 0; i < count; ++i) {
        pid_t tid = tids[i];
        if (ptrace(PTRACE_SEIZE, tid, 0, (void *)(uintptr_t)options) != 0) {
            if (errno != ESRCH) {
                char msg[192];
                snprintf(msg, sizeof(msg), "ERROR SWBP PTRACE_SEIZE tid=%d errno=%d (%s)",
                         tid, errno, strerror(errno));
                log_stamp(msg);
            }
            continue;
        }
        hc_add_thread(tid, true, false);
    }

    for (size_t i = 0; i < g_threads_count; ++i) {
        struct hc_thread *thread = &g_threads[i];
        if (ptrace(PTRACE_INTERRUPT, thread->tid, 0, 0) != 0) continue;
        int status = 0;
        if (waitpid(thread->tid, &status, __WALL) == thread->tid && WIFSTOPPED(status))
            thread->stopped = true;
    }

    return g_threads_count > 0 ? 0 : -ESRCH;
}

static pid_t hc_any_stopped_thread(void) {
    for (size_t i = 0; i < g_threads_count; ++i) {
        if (g_threads[i].traced && g_threads[i].stopped) return g_threads[i].tid;
    }
    return 0;
}

static void hc_continue_all(void) {
    for (size_t i = 0; i < g_threads_count; ++i) {
        struct hc_thread *thread = &g_threads[i];
        if (!thread->traced || !thread->stopped) continue;
        if (ptrace(PTRACE_CONT, thread->tid, 0, 0) == 0)
            thread->stopped = false;
    }
}

static int hc_mutate_param(pid_t pid, pid_t tid, uint64_t bp,
                           int cell_x, int cell_y,
                           int *old_x_out, int *old_y_out,
                           int *min_x_out, int *min_y_out) {
    struct hc_pt_regs regs;
    int rc = hc_get_regs(tid, &regs);
    if (rc != 0) return rc;
    if (regs.pc != bp) return 1;

    uint64_t param = regs.regs[0];
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
    close(mem_fd);

    if (old_x_out) *old_x_out = old_x;
    if (old_y_out) *old_y_out = old_y;
    if (min_x_out) *min_x_out = min_x;
    if (min_y_out) *min_y_out = min_y;

    char msg[512];
    snprintf(msg, sizeof(msg),
             "SWBP hit tid=%d pc=0x%llx x0=0x%llx DeviceParam %dx%d min=%d,%d -> %dx%d write=%s",
             tid, (unsigned long long)regs.pc, (unsigned long long)param,
             old_x, old_y, min_x, min_y, cell_x, cell_y,
             ok ? "ok" : "FAILED");
    log_stamp(msg);
    return ok ? 0 : -EIO;
}

static int hc_step_original_and_rearm(pid_t tid, uint64_t bp,
                                      uint64_t original_word, uint64_t brk_word,
                                      bool rearm) {
    int rc = hc_poke_word(tid, bp, original_word);
    if (rc != 0) return rc;

    if (ptrace(PTRACE_SINGLESTEP, tid, 0, 0) != 0) return -errno;
    int status = 0;
    if (waitpid(tid, &status, __WALL) != tid || !WIFSTOPPED(status)) return -EIO;

    struct hc_thread *thread = hc_find_thread(tid);
    if (thread != NULL) thread->stopped = true;

    if (rearm) {
        rc = hc_poke_word(tid, bp, brk_word);
        if (rc != 0) return rc;
    }
    return 0;
}

static void hc_cleanup(pid_t pid, uint64_t bp, uint64_t original_word, bool brk_installed) {
    pid_t writer = 0;
    for (size_t i = 0; i < g_threads_count; ++i) {
        struct hc_thread *thread = &g_threads[i];
        if (!thread->traced || !hc_tid_in_process(pid, thread->tid)) continue;
        if (!thread->stopped && ptrace(PTRACE_INTERRUPT, thread->tid, 0, 0) == 0) {
            int status = 0;
            if (waitpid(thread->tid, &status, __WALL) == thread->tid && WIFSTOPPED(status))
                thread->stopped = true;
        }
        if (writer == 0 && thread->stopped) writer = thread->tid;
    }

    if (brk_installed && writer != 0) {
        int rc = hc_poke_word(writer, bp, original_word);
        char msg[192];
        snprintf(msg, sizeof(msg), "SWBP cleanup restore tid=%d rc=%d", writer, rc);
        log_stamp(msg);
    }

    for (size_t i = 0; i < g_threads_count; ++i) {
        struct hc_thread *thread = &g_threads[i];
        if (!thread->traced || !thread->stopped || !hc_tid_in_process(pid, thread->tid)) continue;
        (void)ptrace(PTRACE_DETACH, thread->tid, 0, 0);
        thread->traced = false;
        thread->stopped = false;
    }
    memset(g_threads, 0, sizeof(g_threads));
    g_threads_count = 0;
}

static int hc_intercept(pid_t pid, uint64_t base, int cell_x, int cell_y) {
    g_threads_count = 0;
    uint64_t bp = base + RUST_RVA_SAVE_DEVICE_PARAM;

    int mem_fd = open_mem(pid);
    if (mem_fd < 0) return -errno;
    bool signature_ok = hc_save_signature_matches(mem_fd, base);
    close(mem_fd);
    if (!signature_ok) {
        log_stamp("ERROR SWBP save_device_param signature mismatch");
        return 41;
    }

    int rc = hc_seize_and_stop_all(pid);
    if (rc != 0) {
        hc_cleanup(pid, bp, 0, false);
        return 42;
    }

    pid_t writer = hc_any_stopped_thread();
    if (writer == 0) {
        hc_cleanup(pid, bp, 0, false);
        return 43;
    }

    uint64_t original_word = 0;
    rc = hc_read_code_word(pid, bp, &original_word);
    if (rc != 0) {
        hc_cleanup(pid, bp, 0, false);
        return 44;
    }
    uint64_t brk_word = (original_word & 0xffffffff00000000ULL) | (uint64_t)A64_BRK_0;
    rc = hc_poke_word(writer, bp, brk_word);
    if (rc != 0) {
        char msg[192];
        snprintf(msg, sizeof(msg), "ERROR SWBP install addr=0x%llx tid=%d rc=%d errno=%d",
                 (unsigned long long)bp, writer, rc, -rc);
        log_stamp(msg);
        hc_cleanup(pid, bp, original_word, false);
        return 45;
    }

    char msg[256];
    snprintf(msg, sizeof(msg),
             "SWBP installed pid=%d threads=%zu addr=0x%llx original=0x%016llx",
             pid, g_threads_count, (unsigned long long)bp,
             (unsigned long long)original_word);
    log_stamp(msg);

    bool brk_installed = true;
    hc_continue_all();

    int hits = 0;
    bool saw_final_loader_param = false;
    uint64_t deadline = monotonic_ms() + TRACE_WINDOW_MS;

    while (process_exists(pid) && monotonic_ms() < deadline && !saw_final_loader_param) {
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
                snprintf(msg, sizeof(msg), "SWBP clone parent=%d new_tid=%lu", tid, new_tid);
                log_stamp(msg);
            }
            if (ptrace(PTRACE_CONT, tid, 0, 0) == 0) thread->stopped = false;
            continue;
        }

        if (sig == SIGTRAP) {
            struct hc_pt_regs regs;
            if (hc_get_regs(tid, &regs) == 0 && regs.pc == bp) {
                int old_x = 0, old_y = 0, min_x = 0, min_y = 0;
                int mrc = hc_mutate_param(pid, tid, bp, cell_x, cell_y,
                                          &old_x, &old_y, &min_x, &min_y);
                if (mrc == 0) {
                    ++hits;
                    bool final_loader = (min_x == 4 && min_y == -1);
                    bool rearm = !final_loader;
                    rc = hc_step_original_and_rearm(tid, bp, original_word, brk_word, rearm);
                    brk_installed = rearm;
                    if (rc != 0) {
                        snprintf(msg, sizeof(msg), "ERROR SWBP single-step/rearm tid=%d rc=%d", tid, rc);
                        log_stamp(msg);
                        break;
                    }
                    saw_final_loader_param = final_loader;
                    snprintf(msg, sizeof(msg),
                             "SWBP consumed tid=%d hit=%d final_loader=%s rearm=%s",
                             tid, hits, final_loader ? "true" : "false", rearm ? "true" : "false");
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

    snprintf(msg, sizeof(msg), "SWBP interception window done pid=%d hits=%d final_loader=%s",
             pid, hits, saw_final_loader_param ? "true" : "false");
    log_stamp(msg);
    hc_cleanup(pid, bp, original_word, brk_installed);
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
             "native SWBP watcher ready: grid=%s cell=%dx%d pid=%d; PTRACE_POKETEXT I-cache path",
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
                snprintf(msg, sizeof(msg), "launcher pid=%d exited; SWBP re-armed", tracked);
                log_stamp(msg);
                tracked = 0;
                attempted = false;
            }
            usleep(1000);
            continue;
        }

        if (pid != tracked) {
            tracked = pid;
            attempted = !grid;
            snprintf(msg, sizeof(msg), "launcher candidate detected pid=%d", pid);
            log_stamp(msg);
        }

        if (grid && !attempted) {
            int mem_fd = open_mem(pid);
            if (mem_fd >= 0) {
                uint64_t base = 0;
                if (find_rust_base(pid, mem_fd, &base)) {
                    close(mem_fd);
                    snprintf(msg, sizeof(msg), "Rust SWBP target mapped pid=%d base=0x%llx",
                             pid, (unsigned long long)base);
                    log_stamp(msg);
                    int rc = hc_intercept(pid, base, cell_x, cell_y);
                    snprintf(msg, sizeof(msg), "SWBP DeviceParam interception finished pid=%d rc=%d",
                             pid, rc);
                    log_stamp(msg);
                    attempted = true;
                    continue;
                }
                close(mem_fd);
            }
        }
        usleep(attempted ? 100000 : 500);
    }
}
