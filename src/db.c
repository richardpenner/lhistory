#include "db.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static const char *SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS commands ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  session_id TEXT NOT NULL,"
    "  command TEXT NOT NULL,"
    "  timestamp INTEGER NOT NULL,"
    "  directory TEXT NOT NULL,"
    "  exit_code INTEGER,"
    "  shell TEXT NOT NULL,"
    "  ide TEXT"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_commands_timestamp ON commands(timestamp DESC);"
    "CREATE INDEX IF NOT EXISTS idx_commands_session ON commands(session_id);"
    "CREATE TABLE IF NOT EXISTS sessions ("
    "  session_id TEXT PRIMARY KEY,"
    "  shell TEXT NOT NULL,"
    "  ide TEXT,"
    "  started_at INTEGER NOT NULL,"
    "  last_active INTEGER NOT NULL,"
    "  directory TEXT NOT NULL"
    ");";

static int64_t now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

char db_path_buf[1024];

const char *lh_db_path(void) {
    if (db_path_buf[0]) return db_path_buf;

    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0]) {
        snprintf(db_path_buf, sizeof(db_path_buf), "%s/lhistory/history.db", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home) return NULL;
        snprintf(db_path_buf, sizeof(db_path_buf), "%s/.local/share/lhistory/history.db", home);
    }

    /* Override for testing */
    const char *override = getenv("LHISTORY_DB_PATH");
    if (override && override[0]) {
        snprintf(db_path_buf, sizeof(db_path_buf), "%s", override);
    }

    return db_path_buf;
}

static int ensure_dir(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *slash = strrchr(tmp, '/');
    if (!slash) return -1;
    *slash = '\0';

    struct stat st;
    if (stat(tmp, &st) == 0) return 0;

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static void prune_if_oversized(sqlite3 *db) {
    /* Prune when DB exceeds 10MB: delete oldest 25% of commands */
    int64_t page_count = 0, page_size = 0;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "PRAGMA page_count", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) page_count = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }
    if (sqlite3_prepare_v2(db, "PRAGMA page_size", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) page_size = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }
    if (page_count * page_size < 10 * 1024 * 1024) return;

    sqlite3_exec(db,
        "DELETE FROM commands WHERE id IN "
        "(SELECT id FROM commands ORDER BY timestamp ASC, id ASC "
        "LIMIT (SELECT COUNT(*) / 4 FROM commands))",
        NULL, NULL, NULL);
    /* Remove orphaned sessions */
    sqlite3_exec(db,
        "DELETE FROM sessions WHERE session_id NOT IN "
        "(SELECT DISTINCT session_id FROM commands)", NULL, NULL, NULL);
}

sqlite3 *lh_db_open(void) {
    const char *path = lh_db_path();
    if (!path) return NULL;

    int is_new = (access(path, F_OK) != 0);

    if (ensure_dir(path) != 0) {
        fprintf(stderr, "lhistory: cannot create directory for %s\n", path);
        return NULL;
    }

    sqlite3 *db = NULL;
    int rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }

    if (is_new) chmod(path, 0600);

    /* WAL mode for concurrent access */
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA busy_timeout=1000;", NULL, NULL, NULL);

    char *err = NULL;
    rc = sqlite3_exec(db, SCHEMA_SQL, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "lhistory: schema error: %s\n", err ? err : "unknown");
        sqlite3_free(err);
        sqlite3_close(db);
        return NULL;
    }

    prune_if_oversized(db);

    return db;
}

int lh_db_record(sqlite3 *db, const char *session_id, const char *command,
                 const char *directory, const char *shell, const char *ide) {
    int64_t ts = now_ms();

    const char *sql = "INSERT INTO commands (session_id, command, timestamp, directory, shell, ide) "
                      "VALUES (?, ?, ?, ?, ?, ?)";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return rc;

    sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, command, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, ts);
    sqlite3_bind_text(stmt, 4, directory, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, shell, -1, SQLITE_STATIC);
    if (ide)
        sqlite3_bind_text(stmt, 6, ide, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 6);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) return rc;

    lh_db_touch_session(db, session_id, shell, ide, directory);

    return 0;
}

int lh_db_finish(sqlite3 *db, const char *session_id, int exit_code) {
    const char *sql = "UPDATE commands SET exit_code = ? "
                      "WHERE id = (SELECT id FROM commands WHERE session_id = ? "
                      "ORDER BY timestamp DESC, id DESC LIMIT 1)";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return rc;

    sqlite3_bind_int(stmt, 1, exit_code);
    sqlite3_bind_text(stmt, 2, session_id, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : rc;
}

int lh_db_touch_session(sqlite3 *db, const char *session_id, const char *shell,
                        const char *ide, const char *directory) {
    int64_t ts = now_ms();

    const char *sql =
        "INSERT INTO sessions (session_id, shell, ide, started_at, last_active, directory) "
        "VALUES (?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(session_id) DO UPDATE SET last_active = excluded.last_active, "
        "directory = excluded.directory";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return rc;

    sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, shell, -1, SQLITE_STATIC);
    if (ide)
        sqlite3_bind_text(stmt, 3, ide, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 3);
    sqlite3_bind_int64(stmt, 4, ts);
    sqlite3_bind_int64(stmt, 5, ts);
    sqlite3_bind_text(stmt, 6, directory, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : rc;
}

/* Helper to grow a command list */
static int cmd_list_push(lh_command_list *list, lh_command *cmd) {
    if (list->count >= list->capacity) {
        int new_cap = list->capacity ? list->capacity * 2 : 64;
        lh_command *tmp = realloc(list->items, (size_t)new_cap * sizeof(lh_command));
        if (!tmp) return -1;
        list->items = tmp;
        list->capacity = new_cap;
    }
    list->items[list->count++] = *cmd;
    return 0;
}

static int sess_list_push(lh_session_list *list, lh_session *s) {
    if (list->count >= list->capacity) {
        int new_cap = list->capacity ? list->capacity * 2 : 16;
        lh_session *tmp = realloc(list->items, (size_t)new_cap * sizeof(lh_session));
        if (!tmp) return -1;
        list->items = tmp;
        list->capacity = new_cap;
    }
    list->items[list->count++] = *s;
    return 0;
}

static char *safe_strdup(const char *s) {
    return s ? strdup(s) : NULL;
}

static lh_command read_command_row(sqlite3_stmt *stmt) {
    lh_command cmd = {0};
    cmd.id = sqlite3_column_int64(stmt, 0);
    cmd.session_id = safe_strdup((const char *)sqlite3_column_text(stmt, 1));
    cmd.command = safe_strdup((const char *)sqlite3_column_text(stmt, 2));
    cmd.timestamp = sqlite3_column_int64(stmt, 3);
    cmd.directory = safe_strdup((const char *)sqlite3_column_text(stmt, 4));
    if (sqlite3_column_type(stmt, 5) != SQLITE_NULL) {
        cmd.exit_code = sqlite3_column_int(stmt, 5);
        cmd.has_exit_code = 1;
    }
    cmd.shell = safe_strdup((const char *)sqlite3_column_text(stmt, 6));
    cmd.ide = safe_strdup((const char *)sqlite3_column_text(stmt, 7));
    return cmd;
}

lh_session_list lh_db_recent_sessions(sqlite3 *db, int limit) {
    lh_session_list list = {0};

    const char *sql = "SELECT session_id, shell, ide, started_at, last_active, directory "
                      "FROM sessions ORDER BY last_active DESC LIMIT ?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return list;
    sqlite3_bind_int(stmt, 1, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        lh_session s = {0};
        s.session_id = safe_strdup((const char *)sqlite3_column_text(stmt, 0));
        s.shell = safe_strdup((const char *)sqlite3_column_text(stmt, 1));
        s.ide = safe_strdup((const char *)sqlite3_column_text(stmt, 2));
        s.started_at = sqlite3_column_int64(stmt, 3);
        s.last_active = sqlite3_column_int64(stmt, 4);
        s.directory = safe_strdup((const char *)sqlite3_column_text(stmt, 5));
        sess_list_push(&list, &s);
    }
    sqlite3_finalize(stmt);
    return list;
}

lh_command_list lh_db_session_commands(sqlite3 *db, const char *session_id, int limit) {
    lh_command_list list = {0};

    const char *sql = "SELECT id, session_id, command, timestamp, directory, exit_code, shell, ide "
                      "FROM commands WHERE session_id = ? ORDER BY timestamp DESC, id DESC LIMIT ?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return list;
    sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        lh_command cmd = read_command_row(stmt);
        cmd_list_push(&list, &cmd);
    }
    sqlite3_finalize(stmt);
    return list;
}

lh_command_list lh_db_recent_commands(sqlite3 *db, int limit) {
    lh_command_list list = {0};

    const char *sql = "SELECT id, session_id, command, timestamp, directory, exit_code, shell, ide "
                      "FROM commands ORDER BY timestamp DESC, id DESC LIMIT ?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return list;
    sqlite3_bind_int(stmt, 1, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        lh_command cmd = read_command_row(stmt);
        cmd_list_push(&list, &cmd);
    }
    sqlite3_finalize(stmt);
    return list;
}

lh_command_list lh_db_search(sqlite3 *db, const char *pattern, int limit) {
    lh_command_list list = {0};

    const char *sql = "SELECT id, session_id, command, timestamp, directory, exit_code, shell, ide "
                      "FROM commands WHERE command LIKE ? ORDER BY timestamp DESC, id DESC LIMIT ?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return list;

    size_t plen = strlen(pattern);
    char *like = malloc(plen + 3);
    if (!like) { sqlite3_finalize(stmt); return list; }
    like[0] = '%';
    memcpy(like + 1, pattern, plen);
    like[plen + 1] = '%';
    like[plen + 2] = '\0';

    sqlite3_bind_text(stmt, 1, like, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        lh_command cmd = read_command_row(stmt);
        cmd_list_push(&list, &cmd);
    }
    sqlite3_finalize(stmt);
    free(like);
    return list;
}

void lh_command_list_free(lh_command_list *list) {
    for (int i = 0; i < list->count; i++) {
        free(list->items[i].session_id);
        free(list->items[i].command);
        free(list->items[i].directory);
        free(list->items[i].shell);
        free(list->items[i].ide);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void lh_db_compute_stats(sqlite3 *db, lh_db_stats *out) {
    memset(out, 0, sizeof(*out));
    out->busiest_hour = -1;

    int64_t now = now_ms();
    int64_t day_ms = 86400LL * 1000;
    int64_t thresholds[] = {now - day_ms, now - 7 * day_ms, now - 30 * day_ms, 0};
    int *targets[] = {&out->commands_today, &out->commands_7d, &out->commands_30d, &out->commands_all};

    for (int i = 0; i < 4; i++) {
        sqlite3_stmt *stmt;
        const char *sql = thresholds[i]
            ? "SELECT COUNT(*) FROM commands WHERE timestamp >= ?"
            : "SELECT COUNT(*) FROM commands";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) continue;
        if (thresholds[i]) sqlite3_bind_int64(stmt, 1, thresholds[i]);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            *targets[i] = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }

    {
        sqlite3_stmt *stmt;
        const char *sql = "SELECT substr(command, 1, instr(command||' ',' ')-1), COUNT(*) as c "
                          "FROM commands GROUP BY 1 ORDER BY c DESC LIMIT 3";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW && out->top_count < 3) {
                const char *w = (const char *)sqlite3_column_text(stmt, 0);
                if (w) snprintf(out->top_commands[out->top_count], 64, "%s", w);
                out->top_count++;
            }
            sqlite3_finalize(stmt);
        }
    }

    {
        sqlite3_stmt *stmt;
        const char *sql = "SELECT CAST(strftime('%H', timestamp/1000, 'unixepoch', 'localtime') AS INTEGER), "
                          "COUNT(*) as c FROM commands GROUP BY 1 ORDER BY c DESC LIMIT 1";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW)
                out->busiest_hour = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
        }
    }

    {
        sqlite3_stmt *stmt;
        const char *sql = "SELECT substr(command, 1, instr(command||' ',' ')-1), COUNT(*) as c "
                          "FROM commands WHERE exit_code = 127 GROUP BY 1 ORDER BY c DESC LIMIT 1";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *w = (const char *)sqlite3_column_text(stmt, 0);
                if (w) snprintf(out->top_typo, sizeof(out->top_typo), "%s", w);
                out->typo_count = sqlite3_column_int(stmt, 1);
            }
            sqlite3_finalize(stmt);
        }
    }
}

void lh_session_list_free(lh_session_list *list) {
    for (int i = 0; i < list->count; i++) {
        free(list->items[i].session_id);
        free(list->items[i].shell);
        free(list->items[i].ide);
        free(list->items[i].directory);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}
