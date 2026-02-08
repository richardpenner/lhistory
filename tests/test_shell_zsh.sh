#!/bin/zsh
# Test zsh integration for lhistory

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BINARY="$PROJECT_DIR/lhistory"
TEST_DB="/tmp/lhistory_test_zsh_$$.db"

export LHISTORY_DB_PATH="$TEST_DB"
export LHISTORY_SID="test-zsh-$$"

cleanup() {
    rm -f "$TEST_DB" "${TEST_DB}-wal" "${TEST_DB}-shm"
}
trap cleanup EXIT

passed=0
failed=0

pass() { echo "PASS"; passed=$((passed + 1)); }
fail() { echo "FAIL: $1"; failed=$((failed + 1)); }

echo "test_shell_zsh:"

# Test 1: Record a command
echo -n "  record command via CLI                            "
"$BINARY" record --sid "$LHISTORY_SID" --cmd "echo hello" --dir "/tmp" --shell zsh
count=$(sqlite3 "$TEST_DB" "SELECT COUNT(*) FROM commands WHERE session_id='$LHISTORY_SID'")
[[ "$count" == "1" ]] && pass || fail "expected count 1, got $count"

# Test 2: Record another command and verify ordering
echo -n "  multiple commands maintain order                   "
"$BINARY" record --sid "$LHISTORY_SID" --cmd "ls -la" --dir "/tmp" --shell zsh
count=$(sqlite3 "$TEST_DB" "SELECT COUNT(*) FROM commands WHERE session_id='$LHISTORY_SID'")
last_cmd=$(sqlite3 "$TEST_DB" "SELECT command FROM commands WHERE session_id='$LHISTORY_SID' ORDER BY id DESC LIMIT 1")
[[ "$count" == "2" && "$last_cmd" == "ls -la" ]] && pass || fail "count=$count last_cmd=$last_cmd"

# Test 3: Finish with exit code
echo -n "  finish records exit code                          "
"$BINARY" finish --sid "$LHISTORY_SID" --exit-code 0
exit_code=$(sqlite3 "$TEST_DB" "SELECT exit_code FROM commands WHERE session_id='$LHISTORY_SID' ORDER BY id DESC LIMIT 1")
[[ "$exit_code" == "0" ]] && pass || fail "expected exit_code 0, got $exit_code"

# Test 4: Session record was created
echo -n "  session record exists                             "
sess_count=$(sqlite3 "$TEST_DB" "SELECT COUNT(*) FROM sessions WHERE session_id='$LHISTORY_SID'")
[[ "$sess_count" == "1" ]] && pass || fail "expected 1 session, got $sess_count"

# Test 5: Multiple sessions
echo -n "  multiple sessions tracked                         "
"$BINARY" record --sid "other-session" --cmd "pwd" --dir "/home" --shell zsh --ide cursor
sess_total=$(sqlite3 "$TEST_DB" "SELECT COUNT(*) FROM sessions")
[[ "$sess_total" -ge 2 ]] && pass || fail "expected >= 2 sessions, got $sess_total"

echo ""
echo "$passed passed, $failed failed"
exit $failed
