#!/bin/bash
# Test fish integration for lhistory (uses bash to drive the tests)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BINARY="$PROJECT_DIR/lhistory"
TEST_DB="/tmp/lhistory_test_fish_$$.db"

export LHISTORY_DB_PATH="$TEST_DB"
export LHISTORY_SID="test-fish-$$"

cleanup() {
    rm -f "$TEST_DB" "${TEST_DB}-wal" "${TEST_DB}-shm"
}
trap cleanup EXIT

passed=0
failed=0

pass() { echo "PASS"; passed=$((passed + 1)); }
fail() { echo "FAIL: $1"; failed=$((failed + 1)); }

echo "test_shell_fish:"

# Test 1: Record a command with fish shell type
echo -n "  record command as fish shell                      "
"$BINARY" record --sid "$LHISTORY_SID" --cmd "set -x FOO bar" --dir "/tmp" --shell fish
count=$(sqlite3 "$TEST_DB" "SELECT COUNT(*) FROM commands WHERE session_id='$LHISTORY_SID' AND shell='fish'")
[[ "$count" == "1" ]] && pass || fail "expected count 1, got $count"

# Test 2: Verify shell type is stored correctly
echo -n "  shell type stored as fish                         "
shell=$(sqlite3 "$TEST_DB" "SELECT shell FROM commands WHERE session_id='$LHISTORY_SID' LIMIT 1")
[[ "$shell" == "fish" ]] && pass || fail "expected 'fish', got '$shell'"

echo ""
echo "$passed passed, $failed failed"
exit $failed
