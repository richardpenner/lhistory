#ifndef LHISTORY_DB_H
#define LHISTORY_DB_H

#include "../vendor/sqlite3.h"
#include <stdint.h>

typedef struct {
    int64_t id;
    char *session_id;
    char *command;
    int64_t timestamp;
    char *directory;
    int exit_code;
    int has_exit_code;
    char *shell;
    char *ide;
} lh_command;

typedef struct {
    char *session_id;
    char *shell;
    char *ide;
    int64_t started_at;
    int64_t last_active;
    char *directory;
} lh_session;

typedef struct {
    lh_command *items;
    int count;
    int capacity;
} lh_command_list;

typedef struct {
    lh_session *items;
    int count;
    int capacity;
} lh_session_list;

/* Open database, create schema if needed. Returns NULL on error. */
sqlite3 *lh_db_open(void);

/* Get the database path (~/.local/share/lhistory/history.db) */
const char *lh_db_path(void);

/* Record a new command. Returns 0 on success. */
int lh_db_record(sqlite3 *db, const char *session_id, const char *command,
                 const char *directory, const char *shell, const char *ide);

/* Record exit code of the most recent command in a session. Returns 0 on success. */
int lh_db_finish(sqlite3 *db, const char *session_id, int exit_code);

/* Ensure a session record exists or update last_active. Returns 0 on success. */
int lh_db_touch_session(sqlite3 *db, const char *session_id, const char *shell,
                        const char *ide, const char *directory);

/* Query recent sessions ordered by last_active DESC. Caller must free with lh_session_list_free. */
lh_session_list lh_db_recent_sessions(sqlite3 *db, int limit);

/* Query commands for a specific session ordered by timestamp DESC. */
lh_command_list lh_db_session_commands(sqlite3 *db, const char *session_id, int limit);

/* Query commands across all recent sessions, ordered by timestamp DESC. */
lh_command_list lh_db_recent_commands(sqlite3 *db, int limit);

/* Search commands matching a pattern across all sessions. */
lh_command_list lh_db_search(sqlite3 *db, const char *pattern, int limit);

/* Free a command list and all its strings. */
void lh_command_list_free(lh_command_list *list);

/* Free a session list and all its strings. */
void lh_session_list_free(lh_session_list *list);

#endif
