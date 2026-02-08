#include "ide.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#ifdef __APPLE__
#include <libproc.h>
#include <sys/sysctl.h>
#endif

static const char *check_env(void) {
    /* Cursor-specific */
    const char *cursor_sid = getenv("CURSOR_SESSION_ID");
    if (cursor_sid && cursor_sid[0]) return "cursor";

    /* TERM_PROGRAM is set by many terminals and IDEs */
    const char *term_prog = getenv("TERM_PROGRAM");
    if (term_prog) {
        if (strcasecmp(term_prog, "vscode") == 0) return "vscode";
        if (strcasecmp(term_prog, "cursor") == 0) return "cursor";
        if (strcasecmp(term_prog, "WezTerm") == 0) return "wezterm";
        if (strcasecmp(term_prog, "iTerm.app") == 0) return "iterm";
        if (strcasecmp(term_prog, "Apple_Terminal") == 0) return "terminal";
        if (strcasecmp(term_prog, "ghostty") == 0) return "ghostty";
        if (strcasecmp(term_prog, "Alacritty") == 0) return "alacritty";
        if (strcasecmp(term_prog, "kitty") == 0) return "kitty";
        if (strcasecmp(term_prog, "tmux") == 0) return "tmux";
    }

    /* VS Code specific */
    const char *vscode_pid = getenv("VSCODE_PID");
    if (vscode_pid && vscode_pid[0]) return "vscode";

    /* Zed */
    const char *zed = getenv("ZED_TERM");
    if (zed && zed[0]) return "zed";

    return NULL;
}

#ifdef __APPLE__
static const char *check_parent_processes(void) {
    pid_t pid = getpid();

    /* Walk up the process tree (max 10 levels) */
    for (int i = 0; i < 10 && pid > 1; i++) {
        struct kinfo_proc info;
        size_t size = sizeof(info);
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};

        if (sysctl(mib, 4, &info, &size, NULL, 0) != 0) break;

        pid_t ppid = info.kp_eproc.e_ppid;

        char name[PROC_PIDPATHINFO_MAXSIZE];
        if (proc_name(ppid, name, sizeof(name)) > 0) {
            if (strstr(name, "Cursor")) return "cursor";
            if (strstr(name, "Code") || strstr(name, "code")) return "vscode";
            if (strstr(name, "zed")) return "zed";
        }

        pid = ppid;
    }
    return NULL;
}
#else
static const char *check_parent_processes(void) {
    /* On Linux, read /proc/<pid>/cmdline walking up ppid */
    pid_t pid = getpid();

    for (int i = 0; i < 10 && pid > 1; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/status", pid);
        FILE *f = fopen(path, "r");
        if (!f) break;

        pid_t ppid = 0;
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "PPid:", 5) == 0) {
                ppid = (pid_t)atoi(line + 5);
                break;
            }
        }
        fclose(f);
        if (ppid <= 1) break;

        /* Check comm */
        snprintf(path, sizeof(path), "/proc/%d/comm", ppid);
        f = fopen(path, "r");
        if (!f) break;
        char comm[256] = {0};
        if (fgets(comm, sizeof(comm), f)) {
            /* strip newline */
            char *nl = strchr(comm, '\n');
            if (nl) *nl = '\0';

            if (strstr(comm, "cursor") || strstr(comm, "Cursor")) { fclose(f); return "cursor"; }
            if (strstr(comm, "code") || strstr(comm, "Code")) { fclose(f); return "vscode"; }
            if (strstr(comm, "zed")) { fclose(f); return "zed"; }
        }
        fclose(f);
        pid = ppid;
    }
    return NULL;
}
#endif

const char *lh_detect_ide(void) {
    const char *result = check_env();
    if (result) return result;

    result = check_parent_processes();
    return result;
}
