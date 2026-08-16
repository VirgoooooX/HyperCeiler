#pragma once

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* Original v12 fallback, defined later in the generated translation unit. */
static pid_t hc_find_spawner_v12(void);

static bool hc_v13_numeric_name(const char *name) {
    if (name == NULL || *name == '\0') return false;
    for (const char *p = name; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') return false;
    }
    return true;
}

static bool hc_v13_file_contains(pid_t pid, const char *leaf, const char *needle) {
    char path[96];
    snprintf(path, sizeof(path), "/proc/%d/%s", pid, leaf);
    FILE *f = fopen(path, "re");
    if (f == NULL) return false;

    char line[1024];
    bool found = false;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strstr(line, needle) != NULL) {
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

static bool hc_v13_comm_equals(pid_t pid, const char *expected) {
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

static pid_t hc_v13_read_ppid(pid_t pid) {
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

/*
 * The persistent HyperOS Rust spawner does not necessarily expose
 * "hyos_spawner" through /proc/PID/comm.  The live launcher is nevertheless
 * its direct child.  Resolve the spawner from the PPid of the currently
 * running Rust launcher while it is still alive, then arm that parent before
 * Java kills/restarts com.miui.home.
 */
static pid_t hc_v13_find_launcher_parent(void) {
    DIR *dir = opendir("/proc");
    if (dir == NULL) return 0;

    pid_t found = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!hc_v13_numeric_name(entry->d_name)) continue;
        pid_t pid = (pid_t)strtol(entry->d_name, NULL, 10);
        if (!hc_v13_comm_equals(pid, "com.miui.home")) continue;
        if (!hc_v13_file_contains(pid, "maps", "libapp_launcher.so")) continue;

        pid_t ppid = hc_v13_read_ppid(pid);
        if (ppid > 1 && kill(ppid, 0) == 0) {
            found = ppid;
            break;
        }
    }
    closedir(dir);
    return found;
}

static pid_t hc_find_spawner_v13(void) {
    pid_t parent = hc_v13_find_launcher_parent();
    if (parent > 1) return parent;

    /* Fallback for builds that really do expose the Rust spawner name. */
    return hc_find_spawner_v12();
}
