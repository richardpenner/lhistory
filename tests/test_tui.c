#include "../src/db.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* TUI tests are non-interactive — we test the data layer that feeds the TUI
 * and verify IDE detection works. Full TUI rendering requires a terminal. */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    printf("  %-50s", #name); \
    name(); \
    printf("PASS\n"); \
    tests_passed++; \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAIL\n    assertion failed: %s\n    at %s:%d\n", #cond, __FILE__, __LINE__); \
        tests_failed++; \
        return; \
    } \
} while(0)

static char test_db_path[256];
static int test_counter = 0;

static void setup(void) {
    snprintf(test_db_path, sizeof(test_db_path), "/tmp/lhistory_tui_test_%d_%d.db", getpid(), test_counter++);
    setenv("LHISTORY_DB_PATH", test_db_path, 1);
    extern char db_path_buf[1024];
    db_path_buf[0] = '\0';
}

static void teardown(void) {
    unlink(test_db_path);
    char wal_path[280], shm_path[280];
    snprintf(wal_path, sizeof(wal_path), "%s-wal", test_db_path);
    snprintf(shm_path, sizeof(shm_path), "%s-shm", test_db_path);
    unlink(wal_path);
    unlink(shm_path);
}

TEST(test_multi_session_data_for_columns) {
    setup();
    sqlite3 *db = lh_db_open();
    ASSERT(db != NULL);

    /* Simulate 3 sessions with interleaved commands */
    lh_db_record(db, "s1", "cd /project", "/home", "zsh", NULL);
    lh_db_record(db, "s2", "npm start", "/app", "bash", "vscode");
    lh_db_record(db, "s3", "cargo build", "/rust", "zsh", "cursor");
    lh_db_record(db, "s1", "make", "/project", "zsh", NULL);
    lh_db_record(db, "s2", "npm test", "/app", "bash", "vscode");

    lh_session_list sessions = lh_db_recent_sessions(db, 16);
    ASSERT(sessions.count == 3);

    /* Verify each session has the right command count */
    for (int i = 0; i < sessions.count; i++) {
        lh_command_list cmds = lh_db_session_commands(db, sessions.items[i].session_id, 500);
        if (strcmp(sessions.items[i].session_id, "s1") == 0) {
            ASSERT(cmds.count == 2);
        } else if (strcmp(sessions.items[i].session_id, "s2") == 0) {
            ASSERT(cmds.count == 2);
        } else if (strcmp(sessions.items[i].session_id, "s3") == 0) {
            ASSERT(cmds.count == 1);
        }
        lh_command_list_free(&cmds);
    }

    lh_session_list_free(&sessions);
    sqlite3_close(db);
    teardown();
}

TEST(test_search_across_sessions) {
    setup();
    sqlite3 *db = lh_db_open();
    ASSERT(db != NULL);

    lh_db_record(db, "s1", "git status", "/a", "zsh", NULL);
    lh_db_record(db, "s2", "git log --oneline", "/b", "bash", NULL);
    lh_db_record(db, "s1", "make clean", "/a", "zsh", NULL);
    lh_db_record(db, "s2", "git diff HEAD", "/b", "bash", NULL);

    lh_command_list results = lh_db_search(db, "git", 100);
    ASSERT(results.count == 3);

    /* Verify they come from different sessions */
    int from_s1 = 0, from_s2 = 0;
    for (int i = 0; i < results.count; i++) {
        if (strcmp(results.items[i].session_id, "s1") == 0) from_s1++;
        if (strcmp(results.items[i].session_id, "s2") == 0) from_s2++;
    }
    ASSERT(from_s1 == 1);
    ASSERT(from_s2 == 2);

    lh_command_list_free(&results);
    sqlite3_close(db);
    teardown();
}

TEST(test_exit_code_coloring_data) {
    setup();
    sqlite3 *db = lh_db_open();
    ASSERT(db != NULL);

    lh_db_record(db, "s1", "true", "/", "zsh", NULL);
    lh_db_finish(db, "s1", 0);

    lh_db_record(db, "s1", "false", "/", "zsh", NULL);
    lh_db_finish(db, "s1", 1);

    lh_db_record(db, "s1", "running", "/", "zsh", NULL);
    /* no finish — still running */

    lh_command_list cmds = lh_db_session_commands(db, "s1", 100);
    ASSERT(cmds.count == 3);

    /* Most recent first: running (no exit), false (exit=1), true (exit=0) */
    ASSERT(!cmds.items[0].has_exit_code);  /* running */
    ASSERT(cmds.items[1].has_exit_code);   /* false */
    ASSERT(cmds.items[1].exit_code == 1);
    ASSERT(cmds.items[2].has_exit_code);   /* true */
    ASSERT(cmds.items[2].exit_code == 0);

    lh_command_list_free(&cmds);
    sqlite3_close(db);
    teardown();
}

TEST(test_ide_detection_env) {
    /* Test that IDE detection reads env vars */
    const char *orig = getenv("CURSOR_SESSION_ID");

    setenv("CURSOR_SESSION_ID", "test-123", 1);
    extern const char *lh_detect_ide(void);
    const char *ide = lh_detect_ide();
    ASSERT(ide != NULL);
    ASSERT(strcmp(ide, "cursor") == 0);

    if (orig)
        setenv("CURSOR_SESSION_ID", orig, 1);
    else
        unsetenv("CURSOR_SESSION_ID");
}

TEST(test_large_history) {
    setup();
    sqlite3 *db = lh_db_open();
    ASSERT(db != NULL);

    /* Insert 1000 commands */
    for (int i = 0; i < 1000; i++) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "command_%d", i);
        lh_db_record(db, "big_session", cmd, "/", "zsh", NULL);
    }

    lh_command_list cmds = lh_db_session_commands(db, "big_session", 500);
    ASSERT(cmds.count == 500);

    /* Verify ordering (most recent first) */
    ASSERT(cmds.items[0].timestamp >= cmds.items[cmds.count - 1].timestamp);

    lh_command_list_free(&cmds);
    sqlite3_close(db);
    teardown();
}

int main(void) {
    printf("test_tui (data layer):\n");
    RUN(test_multi_session_data_for_columns);
    RUN(test_search_across_sessions);
    RUN(test_exit_code_coloring_data);
    RUN(test_ide_detection_env);
    RUN(test_large_history);

    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
