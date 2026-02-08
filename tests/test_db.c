#include "../src/db.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

#define ASSERT_EQ_INT(a, b) do { \
    int _a = (a), _b = (b); \
    if (_a != _b) { \
        printf("FAIL\n    expected %d, got %d\n    at %s:%d\n", _b, _a, __FILE__, __LINE__); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_EQ_STR(a, b) do { \
    const char *_a = (a), *_b = (b); \
    if (strcmp(_a, _b) != 0) { \
        printf("FAIL\n    expected \"%s\", got \"%s\"\n    at %s:%d\n", _b, _a, __FILE__, __LINE__); \
        tests_failed++; \
        return; \
    } \
} while(0)

static char test_db_path[256];
static int test_counter = 0;

static void setup(void) {
    snprintf(test_db_path, sizeof(test_db_path), "/tmp/lhistory_test_%d_%d.db", getpid(), test_counter++);
    setenv("LHISTORY_DB_PATH", test_db_path, 1);
    /* Reset the cached path */
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

TEST(test_db_open_creates_schema) {
    setup();
    sqlite3 *db = lh_db_open();
    ASSERT(db != NULL);

    /* Verify tables exist */
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db,
        "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name", -1, &stmt, NULL);
    ASSERT_EQ_INT(rc, SQLITE_OK);

    int found_commands = 0, found_sessions = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        if (strcmp(name, "commands") == 0) found_commands = 1;
        if (strcmp(name, "sessions") == 0) found_sessions = 1;
    }
    sqlite3_finalize(stmt);
    ASSERT(found_commands);
    ASSERT(found_sessions);

    sqlite3_close(db);
    teardown();
}

TEST(test_record_and_query) {
    setup();
    sqlite3 *db = lh_db_open();
    ASSERT(db != NULL);

    int rc = lh_db_record(db, "sess1", "echo hello", "/home/user", "zsh", NULL);
    ASSERT_EQ_INT(rc, 0);

    rc = lh_db_record(db, "sess1", "ls -la", "/home/user", "zsh", NULL);
    ASSERT_EQ_INT(rc, 0);

    lh_command_list cmds = lh_db_session_commands(db, "sess1", 100);
    ASSERT(cmds.count == 2);
    /* Most recent first */
    ASSERT_EQ_STR(cmds.items[0].command, "ls -la");
    ASSERT_EQ_STR(cmds.items[1].command, "echo hello");
    ASSERT_EQ_STR(cmds.items[0].session_id, "sess1");

    lh_command_list_free(&cmds);
    sqlite3_close(db);
    teardown();
}

TEST(test_finish_sets_exit_code) {
    setup();
    sqlite3 *db = lh_db_open();
    ASSERT(db != NULL);

    lh_db_record(db, "sess1", "false", "/home/user", "zsh", NULL);
    int rc = lh_db_finish(db, "sess1", 1);
    ASSERT_EQ_INT(rc, 0);

    lh_command_list cmds = lh_db_session_commands(db, "sess1", 100);
    ASSERT(cmds.count == 1);
    ASSERT(cmds.items[0].has_exit_code);
    ASSERT_EQ_INT(cmds.items[0].exit_code, 1);

    lh_command_list_free(&cmds);
    sqlite3_close(db);
    teardown();
}

TEST(test_multiple_sessions) {
    setup();
    sqlite3 *db = lh_db_open();
    ASSERT(db != NULL);

    lh_db_record(db, "sess1", "cmd1", "/dir1", "zsh", NULL);
    lh_db_record(db, "sess2", "cmd2", "/dir2", "bash", "vscode");
    lh_db_record(db, "sess1", "cmd3", "/dir1", "zsh", NULL);

    lh_session_list sessions = lh_db_recent_sessions(db, 10);
    ASSERT(sessions.count == 2);

    lh_command_list all = lh_db_recent_commands(db, 100);
    ASSERT(all.count == 3);

    lh_command_list s1 = lh_db_session_commands(db, "sess1", 100);
    ASSERT(s1.count == 2);

    lh_command_list s2 = lh_db_session_commands(db, "sess2", 100);
    ASSERT(s2.count == 1);
    ASSERT_EQ_STR(s2.items[0].ide, "vscode");

    lh_session_list_free(&sessions);
    lh_command_list_free(&all);
    lh_command_list_free(&s1);
    lh_command_list_free(&s2);
    sqlite3_close(db);
    teardown();
}

TEST(test_search) {
    setup();
    sqlite3 *db = lh_db_open();
    ASSERT(db != NULL);

    lh_db_record(db, "sess1", "git status", "/repo", "zsh", NULL);
    lh_db_record(db, "sess1", "git commit -m 'fix'", "/repo", "zsh", NULL);
    lh_db_record(db, "sess1", "make test", "/repo", "zsh", NULL);
    lh_db_record(db, "sess2", "git log", "/other", "bash", NULL);

    lh_command_list results = lh_db_search(db, "git", 100);
    ASSERT(results.count == 3);

    lh_command_list results2 = lh_db_search(db, "make", 100);
    ASSERT(results2.count == 1);

    lh_command_list_free(&results);
    lh_command_list_free(&results2);
    sqlite3_close(db);
    teardown();
}

TEST(test_session_touch) {
    setup();
    sqlite3 *db = lh_db_open();
    ASSERT(db != NULL);

    int rc = lh_db_touch_session(db, "sess_new", "zsh", "cursor", "/projects");
    ASSERT_EQ_INT(rc, 0);

    lh_session_list sessions = lh_db_recent_sessions(db, 10);
    ASSERT(sessions.count >= 1);

    /* Find our session */
    int found = 0;
    for (int i = 0; i < sessions.count; i++) {
        if (strcmp(sessions.items[i].session_id, "sess_new") == 0) {
            ASSERT_EQ_STR(sessions.items[i].shell, "zsh");
            ASSERT_EQ_STR(sessions.items[i].ide, "cursor");
            ASSERT_EQ_STR(sessions.items[i].directory, "/projects");
            found = 1;
            break;
        }
    }
    ASSERT(found);

    lh_session_list_free(&sessions);
    sqlite3_close(db);
    teardown();
}

TEST(test_empty_database) {
    setup();
    sqlite3 *db = lh_db_open();
    ASSERT(db != NULL);

    lh_session_list sessions = lh_db_recent_sessions(db, 10);
    ASSERT(sessions.count == 0);

    lh_command_list cmds = lh_db_recent_commands(db, 100);
    ASSERT(cmds.count == 0);

    lh_command_list search = lh_db_search(db, "anything", 100);
    ASSERT(search.count == 0);

    lh_session_list_free(&sessions);
    lh_command_list_free(&cmds);
    lh_command_list_free(&search);
    sqlite3_close(db);
    teardown();
}

int main(void) {
    printf("test_db:\n");
    RUN(test_db_open_creates_schema);
    RUN(test_record_and_query);
    RUN(test_finish_sets_exit_code);
    RUN(test_multiple_sessions);
    RUN(test_search);
    RUN(test_session_touch);
    RUN(test_empty_database);

    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
