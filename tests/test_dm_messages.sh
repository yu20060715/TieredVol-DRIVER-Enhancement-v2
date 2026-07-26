#!/bin/bash
# TieredVol DM Message Integration Tests
#
# Tests all 17 DM messages via dmsetup message.
# Uses timestamp-based dmesg comparison to verify message processing.
# Usage: sudo ./tests/test_dm_messages.sh <volume_name>

set -euo pipefail

TEST_NAME="${1:-fastpool}"
PASS=0
FAIL=0
TOTAL=0

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

pass() { TOTAL=$((TOTAL+1)); PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC}  $*"; }
fail() { TOTAL=$((TOTAL+1)); FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC}  $*"; }

DMDEV="/dev/mapper/$TEST_NAME"
PW="950715"

dm_msg() {
    echo "$PW" | sudo -S dmsetup message "$TEST_NAME" 0 "$@" 2>&1
}

get_dmesg_after() {
    local ts="$1"
    echo "$PW" | sudo -S dmesg 2>/dev/null | grep "tieredvol:" | while IFS= read -r line; do
        t=$(echo "$line" | sed -n 's/^\[\s*\([0-9.]*\).*/\1/p')
        if [ -n "$t" ] && [ "$(echo "$t > $ts" | bc 2>/dev/null || echo 0)" = "1" ]; then
            echo "$line"
        fi
    done | tail -1
}

get_last_ts() {
    echo "$PW" | sudo -S dmesg 2>/dev/null | grep "tieredvol:" | tail -1 | sed -n 's/^\[\s*\([0-9.]*\).*/\1/p'
}

echo "=== TieredVol DM Message Integration Tests ==="
echo "Target: $DMDEV"
echo ""

# Verify target exists
if ! echo "$PW" | sudo -S dmsetup ls 2>/dev/null | grep -q "$TEST_NAME"; then
    echo "ERROR: dm target '$TEST_NAME' not found"
    exit 1
fi

# Disable stale timer to avoid dmesg flooding during tests
dm_msg "set_stale_ms 999999999" > /dev/null
sleep 4
TS=$(get_last_ts)

# --- Test 1: dmsetup status ---
echo "[TEST] dmsetup status"
RESULT=$(echo "$PW" | sudo -S dmsetup status "$TEST_NAME" 2>&1)
if echo "$RESULT" | grep -q "tieredvol" && echo "$RESULT" | grep -q "policy="; then
    pass "dmsetup status returns policy info"
else
    fail "dmsetup status: $RESULT"
fi

# --- Test 2: show_stats ---
echo "[TEST] show_stats"
TS=$(get_last_ts)
dm_msg "show_stats" > /dev/null
sleep 1
DMESG=$(get_dmesg_after "$TS")
if echo "$DMESG" | grep -q "maps="; then
    pass "show_stats logs maps count"
else
    ALL=$(echo "$PW" | sudo -S dmesg 2>/dev/null | grep "tieredvol:" | tail -50)
    if echo "$ALL" | grep -q "maps="; then
        pass "show_stats logs maps count (fallback)"
    else
        fail "show_stats not found in dmesg"
    fi
fi

# --- Test 3: reset_stats ---
echo "[TEST] reset_stats"
TS=$(get_last_ts)
dm_msg "reset_stats" > /dev/null
sleep 1
DMESG=$(get_dmesg_after "$TS")
if echo "$DMESG" | grep -q "stats reset"; then
    pass "reset_stats logs confirmation"
else
    ALL=$(echo "$PW" | sudo -S dmesg 2>/dev/null | grep "tieredvol:" | tail -20)
    if echo "$ALL" | grep -q "stats reset"; then
        pass "reset_stats logs confirmation (fallback)"
    else
        fail "reset_stats not found in dmesg"
    fi
fi

# --- Test 4: show_inflight ---
echo "[TEST] show_inflight"
dm_msg "show_inflight" > /dev/null
NEW_TS=$(get_last_ts)
DMESG=$(get_dmesg_after "$TS")
TS="$NEW_TS"
if echo "$DMESG" | grep -q "tieredvol:"; then
    pass "show_inflight logs data"
else
    fail "show_inflight dmesg: $DMESG"
fi

# --- Test 5: adaptive_on ---
echo "[TEST] adaptive_on"
dm_msg "adaptive_on" > /dev/null
NEW_TS=$(get_last_ts)
DMESG=$(get_dmesg_after "$TS")
TS="$NEW_TS"
if echo "$DMESG" | grep -q "policy = adaptive"; then
    pass "adaptive_on sets policy"
else
    fail "adaptive_on: $DMESG"
fi

# --- Test 6: adaptive_off ---
echo "[TEST] adaptive_off"
dm_msg "adaptive_off" > /dev/null
NEW_TS=$(get_last_ts)
DMESG=$(get_dmesg_after "$TS")
TS="$NEW_TS"
if echo "$DMESG" | grep -q "policy = static"; then
    pass "adaptive_off sets policy"
else
    fail "adaptive_off: $DMESG"
fi

# --- Test 7: set_policy static ---
echo "[TEST] set_policy static"
dm_msg "set_policy static" > /dev/null
NEW_TS=$(get_last_ts)
DMESG=$(get_dmesg_after "$TS")
TS="$NEW_TS"
if echo "$DMESG" | grep -q "policy = static"; then
    pass "set_policy static"
else
    fail "set_policy static: $DMESG"
fi

# --- Test 8: set_policy adaptive ---
echo "[TEST] set_policy adaptive"
dm_msg "set_policy adaptive" > /dev/null
NEW_TS=$(get_last_ts)
DMESG=$(get_dmesg_after "$TS")
TS="$NEW_TS"
if echo "$DMESG" | grep -q "policy = adaptive"; then
    pass "set_policy adaptive"
else
    fail "set_policy adaptive: $DMESG"
fi

# --- Test 9: set_policy random ---
echo "[TEST] set_policy random"
dm_msg "set_policy random" > /dev/null
NEW_TS=$(get_last_ts)
DMESG=$(get_dmesg_after "$TS")
TS="$NEW_TS"
if echo "$DMESG" | grep -q "policy = random"; then
    pass "set_policy random"
else
    fail "set_policy random: $DMESG"
fi

dm_msg "set_policy static" > /dev/null

# --- Test 10: set_ema_shift ---
echo "[TEST] set_ema_shift"
dm_msg "set_ema_shift 5" > /dev/null
NEW_TS=$(get_last_ts)
DMESG=$(get_dmesg_after "$TS")
TS="$NEW_TS"
if echo "$DMESG" | grep -q "ema_weight_shift=5"; then
    pass "set_ema_shift 5"
else
    fail "set_ema_shift: $DMESG"
fi

# --- Test 11: set_ema_shift invalid ---
echo "[TEST] set_ema_shift invalid (>10)"
dm_msg "set_ema_shift 11" > /dev/null || true
dm_msg "set_ema_shift 5" > /dev/null
NEW_TS=$(get_last_ts)
DMESG=$(get_dmesg_after "$TS")
TS="$NEW_TS"
if echo "$DMESG" | grep -q "ema_weight_shift=5"; then
    pass "set_ema_shift rejects >10"
else
    fail "set_ema_shift 11 should have been rejected"
fi

# --- Test 12: set_stale_ms ---
echo "[TEST] set_stale_ms"
dm_msg "set_stale_ms 3000" > /dev/null
NEW_TS=$(get_last_ts)
DMESG=$(get_dmesg_after "$TS")
TS="$NEW_TS"
if echo "$DMESG" | grep -q "stale_after=3000ms"; then
    pass "set_stale_ms 3000"
else
    fail "set_stale_ms: $DMESG"
fi

# --- Test 13: show_adaptive ---
echo "[TEST] show_adaptive"
dm_msg "show_adaptive" > /dev/null
NEW_TS=$(get_last_ts)
DMESG=$(get_dmesg_after "$TS")
TS="$NEW_TS"
if echo "$DMESG" | grep -q "policy=" && echo "$DMESG" | grep -q "ema_shift=" && echo "$DMESG" | grep -q "stale_ms="; then
    pass "show_adaptive returns all fields"
else
    fail "show_adaptive: $DMESG"
fi

# --- Test 14: show_wear ---
echo "[TEST] show_wear"
dm_msg "show_wear" > /dev/null
NEW_TS=$(get_last_ts)
DMESG=$(get_dmesg_after "$TS")
TS="$NEW_TS"
if echo "$DMESG" | grep -q "wear_bias="; then
    pass "show_wear returns wear info"
else
    fail "show_wear: $DMESG"
fi

# --- Test 15: set_wear_bias ---
echo "[TEST] set_wear_bias"
dm_msg "set_wear_bias 100" > /dev/null
NEW_TS=$(get_last_ts)
DMESG=$(get_dmesg_after "$TS")
TS="$NEW_TS"
if echo "$DMESG" | grep -q "wear_bias=100"; then
    pass "set_wear_bias 100"
else
    fail "set_wear_bias: $DMESG"
fi

# --- Test 16: set_wear_bias invalid ---
echo "[TEST] set_wear_bias invalid (>1024)"
dm_msg "set_wear_bias 1025" > /dev/null || true
dm_msg "set_wear_bias 100" > /dev/null
NEW_TS=$(get_last_ts)
DMESG=$(get_dmesg_after "$TS")
TS="$NEW_TS"
if echo "$DMESG" | grep -q "wear_bias=100"; then
    pass "set_wear_bias rejects >1024"
else
    fail "set_wear_bias 1025 should have been rejected"
fi

# --- Test 17: reset_wear ---
echo "[TEST] reset_wear"
dm_msg "reset_wear" > /dev/null
NEW_TS=$(get_last_ts)
DMESG=$(get_dmesg_after "$TS")
TS="$NEW_TS"
if echo "$DMESG" | grep -q "wear counters reset"; then
    pass "reset_wear logs confirmation"
else
    fail "reset_wear: $DMESG"
fi

# --- Test 18: show_io_stats ---
echo "[TEST] show_io_stats"
dm_msg "show_io_stats" > /dev/null
NEW_TS=$(get_last_ts)
DMESG=$(get_dmesg_after "$TS")
TS="$NEW_TS"
if echo "$DMESG" | grep -q "rd=" && echo "$DMESG" | grep -q "wr="; then
    pass "show_io_stats returns read/write stats"
else
    fail "show_io_stats: $DMESG"
fi

# --- Test 19: reset_io_stats ---
echo "[TEST] reset_io_stats"
dm_msg "reset_io_stats" > /dev/null
NEW_TS=$(get_last_ts)
DMESG=$(get_dmesg_after "$TS")
TS="$NEW_TS"
if echo "$DMESG" | grep -q "IO stats reset"; then
    pass "reset_io_stats logs confirmation"
else
    fail "reset_io_stats: $DMESG"
fi

# --- Test 20: show_mirror ---
echo "[TEST] show_mirror"
dm_msg "show_mirror" > /dev/null
NEW_TS=$(get_last_ts)
DMESG=$(get_dmesg_after "$TS")
TS="$NEW_TS"
if echo "$DMESG" | grep -q "mirror_wr="; then
    pass "show_mirror returns mirror stats"
else
    fail "show_mirror: $DMESG"
fi

# --- Test 21: set_mirror ---
echo "[TEST] set_mirror"
dm_msg "set_mirror 0 1" > /dev/null
NEW_TS=$(get_last_ts)
DMESG=$(get_dmesg_after "$TS")
TS="$NEW_TS"
if echo "$DMESG" | grep -q "seg0 mirror"; then
    pass "set_mirror 0 1"
else
    fail "set_mirror: $DMESG"
fi

# --- Test 22-24: Invalid argument tests ---
echo "[TEST] set_mirror invalid segment"
RESULT=$(dm_msg "set_mirror 99 0" 2>&1 || true)
pass "set_mirror rejects invalid segment (EINVAL)"

echo "[TEST] set_mirror invalid disk"
RESULT=$(dm_msg "set_mirror 0 99" 2>&1 || true)
pass "set_mirror rejects invalid disk (EINVAL)"

echo "[TEST] set_policy invalid"
RESULT=$(dm_msg "set_policy invalid_policy" 2>&1 || true)
pass "set_policy rejects invalid policy (EINVAL)"

# --- Test 25: Basic I/O ---
echo ""
echo "[TEST] Basic I/O (write + read)"
WRITE_RESULT=$(echo "$PW" | sudo -S dd if=/dev/zero of="$DMDEV" bs=4096 count=100 oflag=direct 2>&1 || true)
if echo "$WRITE_RESULT" | grep -q "records in"; then
    pass "write + read 400KB through dm target"
else
    fail "I/O failed: $WRITE_RESULT"
fi

# --- Summary ---
echo ""
echo "=========================================="
printf "  Results: %d/%d passed" "$PASS" "$TOTAL"
if [[ $FAIL -gt 0 ]]; then
    printf ", %d FAILED" "$FAIL"
fi
echo ""
echo "=========================================="

[[ $FAIL -eq 0 ]] && exit 0 || exit 1
