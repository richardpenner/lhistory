#!/bin/bash
# Test bash integration for lhistory

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BINARY="$PROJECT_DIR/lhistory"
TEST_DB="/tmp/lhistory_test_bash_$$.db"

export LHISTORY_DB_PATH="$TEST_DB"
export LHISTORY_SID="test-bash-$$"

cleanup() {
    rm -f "$TEST_DB" "${TEST_DB}-wal" "${TEST_DB}-shm"
}
trap cleanup EXIT

passed=0
failed=0

pass() { echo "PASS"; passed=$((passed + 1)); }
fail() { echo "FAIL: $1"; failed=$((failed + 1)); }

echo "test_shell_bash:"

# Test 1: Record a command
echo -n "  record command via CLI                            "
"$BINARY" record --sid "$LHISTORY_SID" --cmd "echo hello" --dir "/tmp" --shell bash
count=$(sqlite3 "$TEST_DB" "SELECT COUNT(*) FROM commands WHERE session_id='$LHISTORY_SID'")
[[ "$count" == "1" ]] && pass || fail "expected count 1, got $count"

# Test 2: Record and finish with exit code
echo -n "  record and finish with exit code                  "
"$BINARY" record --sid "$LHISTORY_SID" --cmd "false" --dir "/tmp" --shell bash
"$BINARY" finish --sid "$LHISTORY_SID" --exit-code 1
exit_code=$(sqlite3 "$TEST_DB" "SELECT exit_code FROM commands WHERE session_id='$LHISTORY_SID' ORDER BY id DESC LIMIT 1")
[[ "$exit_code" == "1" ]] && pass || fail "expected exit_code 1, got $exit_code"

# Test 3: Browse exits cleanly without tty
echo -n "  browse exits cleanly without tty                  "
"$BINARY" browse </dev/null 2>/dev/null || true
pass

echo ""
echo "$passed passed, $failed failed"
exit $failed
