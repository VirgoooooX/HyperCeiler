#pragma once

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* Helpers defined later by os4_root_data_probe.c in the generated TU. */
static pid_t hc_find_spawner_v12(void);
static pid_t find_launcher_pid(bool deep_scan);
static bool comm_is_launcher(pid_t pid);
static bool maps_mentions_launcher(pid_t pid);
static bool process_exists(pid_t pid);
static void log_stamp(const char *message);

static pid_t hc_v14_read_ppid(pid_t pid) {
    char path[64];
    char line[256];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *f = fopen(path, "re");
    if (f == NULL) return 0;

    pid_t ppid = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "PPid:", 5) == 0) {
            ppid = (pid_t)strtol(line + 5, NULL, 10);
            break;
        }
    }
    fclose(f);
    return ppid;
}

static void hc_v14_read_comm(pid_t pid, char out[128]) {
    out[0] = '\0';
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    FILE *f = fopen(path, "re");
    if (f == NULL) return;
    if (fgets(out, 128, f) != NULL) {
        char *nl = strchr(out, '\n');
        if (nl != NULL) *nl = '\0';
    }
    fclose(f);
}

static void hc_v14_read_cmdline(pid_t pid, char out[256]) {
    out[0] = '\0';
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    FILE *f = fopen(path, "re");
    if (f == NULL) return;
    size_t n = fread(out, 1, 255, f);
    fclose(f);
    if (n == 0) return;
    out[n] = '\0';
    for (size_t i = 0; i < n; ++i) {
        if (out[i] == '\0') out[i] = ' ';
    }
}

static bool hc_v14_is_critical_parent(pid_t pid) {
    if (pid <= 1) return true;
    char comm[128];
    hc_v14_read_comm(pid, comm);
    return strcmp(comm, "init") == 0 ||
           strcmp(comm, "zygote") == 0 ||
           strcmp(comm, "zygote64") == 0 ||
           strcmp(comm, "system_server") == 0 ||
           strcmp(comm, "servicemanager") == 0 ||
           strcmp(comm, "hwservicemanager") == 0;
}

static void hc_v14_log_candidate(const char *source, pid_t launcher, pid_t parent) {
    char parent_comm[128];
    char parent_cmdline[256];
    char launcher_comm[128];
    hc_v14_read_comm(parent, parent_comm);
    hc_v14_read_cmdline(parent, parent_cmdline);
    hc_v14_read_comm(launcher, launcher_comm);

    char msg[768];
    snprintf(
        msg,
        sizeof(msg),
        "V14 spawner candidate source=%s launcher=%d launcher_comm='%s' parent=%d parent_comm='%s' parent_cmdline='%s' critical=%s",
        source,
        launcher,
        launcher_comm,
        parent,
        parent_comm,
        parent_cmdline,
        hc_v14_is_critical_parent(parent) ? "true" : "false"
    );
    log_stamp(msg);
}

/*
 * Fallback parent discovery independent of /proc/<launcher>/status.  Linux
 * exposes each task's direct children under task/<tid>/children; root can walk
 * those lists and identify whichever persistent process currently owns the
 * Rust launcher child.
 */
static pid_t hc_v14_scan_children_parent(pid_t *launcher_out) {
    DIR *dir = opendir("/proc");
    if (dir == NULL) return 0;

    pid_t found_parent = 0;
    pid_t found_launcher = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && found_parent == 0) {
        char *end = NULL;
        long raw = strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0' || raw <= 1) continue;
        pid_t parent = (pid_t)raw;

        char path[96];
        snprintf(path, sizeof(path), "/proc/%d/task/%d/children", parent, parent);
        FILE *f = fopen(path, "re");
        if (f == NULL) continue;

        char line[4096] = {0};
        if (fgets(line, sizeof(line), f) != NULL) {
            char *cursor = line;
            while (*cursor != '\0') {
                while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n') ++cursor;
                if (*cursor == '\0') break;
                char *next = NULL;
                long child_raw = strtol(cursor, &next, 10);
                if (next == cursor) break;
                cursor = next;
                if (child_raw <= 1) continue;
                pid_t child = (pid_t)child_raw;
                if (comm_is_launcher(child) || maps_mentions_launcher(child)) {
                    found_parent = parent;
                    found_launcher = child;
                    break;
                }
            }
        }
        fclose(f);
    }
    closedir(dir);
    if (launcher_out != NULL) *launcher_out = found_launcher;
    return found_parent;
}

/*
 * v13 required /proc/<pid>/comm == com.miui.home plus a literal
 * libapp_launcher.so maps entry.  Launcher 8 can expose neither shape even
 * though the older deep scanner already finds it reliably through base.apk.
 * Reuse that proven scanner first, then resolve its PPid.  If status PPid is
 * unavailable, walk every process' children list as a second independent path.
 */
static pid_t hc_find_spawner_v14(void) {
    pid_t launcher = find_launcher_pid(true);
    if (launcher > 1) {
        pid_t parent = hc_v14_read_ppid(launcher);
        hc_v14_log_candidate("find_launcher_pid", launcher, parent);
        if (parent > 1 && process_exists(parent) && !hc_v14_is_critical_parent(parent)) {
            return parent;
        }
    } else {
        log_stamp("V14 launcher deep scan found no live launcher candidate");
    }

    pid_t child = 0;
    pid_t parent = hc_v14_scan_children_parent(&child);
    if (parent > 1) {
        hc_v14_log_candidate("children", child, parent);
        if (process_exists(parent) && !hc_v14_is_critical_parent(parent)) {
            return parent;
        }
    }

    pid_t named = hc_find_spawner_v12();
    if (named > 1) {
        hc_v14_log_candidate("hyos_spawner_comm_fallback", 0, named);
        if (!hc_v14_is_critical_parent(named)) return named;
    }

    return 0;
}
