#include "db.h"
#include "ide.h"
#include "tui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

static void usage(void) {
    fprintf(stderr,
        "Usage: lhistory <command> [options]\n"
        "\n"
        "Commands:\n"
        "  record  --sid <id> --cmd <cmd> --dir <dir> --shell <sh> [--ide <ide>]\n"
        "  finish  --sid <id> --exit-code <code>\n"
        "  browse  Launch the TUI history browser\n"
        "  init    <shell>  Output shell integration code\n"
        "  install Append source line to shell rc files\n"
        "\n");
}

static int cmd_record(int argc, char **argv) {
    const char *sid = NULL, *cmd = NULL, *dir = NULL, *shell = NULL, *ide = NULL;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--sid") == 0 && i + 1 < argc) { sid = argv[++i]; }
        else if (strcmp(argv[i], "--cmd") == 0 && i + 1 < argc) { cmd = argv[++i]; }
        else if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) { dir = argv[++i]; }
        else if (strcmp(argv[i], "--shell") == 0 && i + 1 < argc) { shell = argv[++i]; }
        else if (strcmp(argv[i], "--ide") == 0 && i + 1 < argc) { ide = argv[++i]; }
    }

    if (!sid || !cmd || !dir || !shell) {
        fprintf(stderr, "lhistory record: missing required arguments\n");
        return 1;
    }

    /* Auto-detect IDE if not provided */
    if (!ide) ide = lh_detect_ide();

    sqlite3 *db = lh_db_open();
    if (!db) { fprintf(stderr, "lhistory: cannot open database\n"); return 1; }

    int rc = lh_db_record(db, sid, cmd, dir, shell, ide);
    sqlite3_close(db);
    return rc ? 1 : 0;
}

static int cmd_finish(int argc, char **argv) {
    const char *sid = NULL;
    int exit_code = -1;
    int has_exit = 0;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--sid") == 0 && i + 1 < argc) { sid = argv[++i]; }
        else if (strcmp(argv[i], "--exit-code") == 0 && i + 1 < argc) {
            exit_code = atoi(argv[++i]);
            has_exit = 1;
        }
    }

    if (!sid || !has_exit) {
        fprintf(stderr, "lhistory finish: missing required arguments\n");
        return 1;
    }

    sqlite3 *db = lh_db_open();
    if (!db) { fprintf(stderr, "lhistory: cannot open database\n"); return 1; }

    int rc = lh_db_finish(db, sid, exit_code);
    sqlite3_close(db);
    return rc ? 1 : 0;
}

static int cmd_browse(void) {
    sqlite3 *db = lh_db_open();
    if (!db) { fprintf(stderr, "lhistory: cannot open database\n"); return 1; }

    const char *current_sid = getenv("LHISTORY_SID");

    char result[4096] = {0};
    int len = lh_tui_browse(db, result, sizeof(result), current_sid);
    sqlite3_close(db);

    if (len > 0) {
        /* Write selected command to stdout for the shell to pick up */
        printf("%s", result);
    }

    return 0;
}

static int cmd_init(const char *shell) {
    const char *locations[] = {
        NULL, /* filled in with binary dir */
        "/usr/local/share/lhistory",
        "/opt/homebrew/share/lhistory",
    };

    char self_path[1024] = {0};
    char shell_dir[1024] = {0};

#ifdef __APPLE__
    uint32_t bufsize = sizeof(self_path);
    if (_NSGetExecutablePath(self_path, &bufsize) == 0) {
        char *slash = strrchr(self_path, '/');
        if (slash) {
            *slash = '\0';
            snprintf(shell_dir, sizeof(shell_dir), "%s/../share/lhistory", self_path);
            locations[0] = shell_dir;
        }
    }
#else
    ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (len > 0) {
        self_path[len] = '\0';
        char *slash = strrchr(self_path, '/');
        if (slash) {
            *slash = '\0';
            snprintf(shell_dir, sizeof(shell_dir), "%s/../share/lhistory", self_path);
            locations[0] = shell_dir;
        }
    }
#endif

    /* Also check adjacent shell/ directory (development) */
    char dev_dir[1024] = {0};
    if (self_path[0]) {
        snprintf(dev_dir, sizeof(dev_dir), "%s/shell", self_path);
    }

    char filename[32];
    snprintf(filename, sizeof(filename), "lhistory.%s", shell);

    /* Try dev dir first (for development), then installed locations */
    const char *all_locations[] = {
        dev_dir[0] ? dev_dir : NULL,
        locations[0],
        locations[1],
        locations[2],
    };

    for (int i = 0; i < 4; i++) {
        if (!all_locations[i]) continue;
        char path[2048];
        snprintf(path, sizeof(path), "%s/%s", all_locations[i], filename);
        FILE *f = fopen(path, "r");
        if (f) {
            /* Output file contents directly — avoids path injection via source */
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
                fwrite(buf, 1, n, stdout);
            }
            fclose(f);
            return 0;
        }
    }

    /* Fallback: output the path that install would use */
    printf("# lhistory: shell integration file not found for '%s'\n", shell);
    printf("# Run 'lhistory install' to set up shell integration\n");
    return 1;
}

static int cmd_install(void) {
    const char *home = getenv("HOME");
    if (!home) { fprintf(stderr, "lhistory: $HOME not set\n"); return 1; }

    char bin_path[1024] = "lhistory";
#ifdef __APPLE__
    {
        uint32_t bufsize = sizeof(bin_path);
        _NSGetExecutablePath(bin_path, &bufsize);
    }
#else
    {
        ssize_t n = readlink("/proc/self/exe", bin_path, sizeof(bin_path) - 1);
        if (n > 0) bin_path[n] = '\0';
    }
#endif

    const char *shell_env = getenv("SHELL");
    if (!shell_env) shell_env = "/bin/zsh";

    struct {
        const char *name;
        const char *rc;
        const char *eval_fmt; /* shell-specific eval syntax */
    } shells[] = {
        {"zsh",  ".zshrc",                    "\n# lhistory - cross-shell history browser\neval \"$(%s init zsh)\"\n"},
        {"bash", ".bashrc",                   "\n# lhistory - cross-shell history browser\neval \"$(%s init bash)\"\n"},
        {"fish", ".config/fish/config.fish",  "\n# lhistory - cross-shell history browser\neval (%s init fish)\n"},
    };

    int installed = 0;
    for (int i = 0; i < 3; i++) {
        char rc_path[1024];
        snprintf(rc_path, sizeof(rc_path), "%s/%s", home, shells[i].rc);

        /* Only install for shells whose rc files exist, or the current shell */
        if (access(rc_path, F_OK) != 0 && !strstr(shell_env, shells[i].name)) continue;

        /* Check if already sourced */
        FILE *f = fopen(rc_path, "r");
        int already = 0;
        if (f) {
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                if (strstr(line, "lhistory")) { already = 1; break; }
            }
            fclose(f);
        }

        if (already) {
            printf("  %s: already configured (%s)\n", shells[i].name, rc_path);
            continue;
        }

        f = fopen(rc_path, "a");
        if (!f) {
            fprintf(stderr, "  %s: cannot write to %s\n", shells[i].name, rc_path);
            continue;
        }
        fprintf(f, shells[i].eval_fmt, bin_path);
        fclose(f);
        printf("  %s: added to %s\n", shells[i].name, rc_path);
        installed++;
    }

    if (installed == 0) {
        printf("No new shell integrations installed (already configured or no rc files found).\n");
    } else {
        printf("\nRestart your shell or run: source <rc_file>\n");
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 1;
    }

    const char *subcmd = argv[1];

    if (strcmp(subcmd, "record") == 0) {
        return cmd_record(argc - 2, argv + 2);
    } else if (strcmp(subcmd, "finish") == 0) {
        return cmd_finish(argc - 2, argv + 2);
    } else if (strcmp(subcmd, "browse") == 0) {
        return cmd_browse();
    } else if (strcmp(subcmd, "init") == 0) {
        if (argc < 3) {
            fprintf(stderr, "lhistory init: specify shell (zsh, bash, fish)\n");
            return 1;
        }
        return cmd_init(argv[2]);
    } else if (strcmp(subcmd, "install") == 0) {
        return cmd_install();
    } else if (strcmp(subcmd, "--help") == 0 || strcmp(subcmd, "-h") == 0) {
        usage();
        return 0;
    } else if (strcmp(subcmd, "--version") == 0 || strcmp(subcmd, "-v") == 0) {
        printf("lhistory %s\n", LHISTORY_VERSION);
        return 0;
    } else {
        fprintf(stderr, "lhistory: unknown command '%s'\n", subcmd);
        usage();
        return 1;
    }
}
