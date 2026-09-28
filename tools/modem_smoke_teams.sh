#!/bin/sh
# Real-audio smoke test of unlimited_modem (spec 12.8; not part of `make test`).
#
# Two modems, A and B, share the "Microsoft Teams Audio" virtual device, which loops its output back to its input
# sample for sample: each hears the other, and itself, as two radios on one frequency would. KISS frames written into
# A's pseudo-terminal must come out of B's byte for byte, and B's answer out of A's. Silent: both modems use only that
# device, named by its UID, so nothing reaches the speakers and nothing comes from a microphone. The Microsoft Teams
# app must not be running (it would use the device).
#
# Usage: sh tools/modem_smoke_teams.sh [BPS]     (default 6; build first: make modem)
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
MODEM="$ROOT/bin/unlimited_modem"
DEVICE_UID="MSLoopbackDriverDevice_UID"
DEVICE="coreaudio:$DEVICE_UID"
BPS=${1:-6}
TIMEOUT_S=120
WORK=$(mktemp -d "${TMPDIR:-/tmp}/unlimited_smoke.XXXXXX")
MODEMS=""
READERS=""

fail() {
    echo "smoke: FAIL: $*"
    stop_all
    exit 1
}

stop_all() {
    for pid in $MODEMS $READERS; do kill -INT "$pid" 2>/dev/null; done
    for pid in $MODEMS $READERS; do wait "$pid" 2>/dev/null; done
    MODEMS=""
    READERS=""
}

# Waits until the file exists, at most TIMEOUT_S seconds.
wait_for() {
    waited=0
    while [ ! -e "$1" ]; do
        [ "$waited" -ge "$((TIMEOUT_S * 5))" ] && return 1
        sleep 0.2
        waited=$((waited + 1))
    done
}

# Waits until the file holds exactly the expected bytes, at most TIMEOUT_S seconds.
wait_bytes() {
    waited=0
    until cmp -s "$1" "$2"; do
        [ "$waited" -ge "$((TIMEOUT_S * 5))" ] && return 1
        sleep 0.2
        waited=$((waited + 1))
    done
}

[ -x "$MODEM" ] || fail "no $MODEM: run make modem first"
"$MODEM" --list-devices | grep -q "$DEVICE_UID" || fail "the Microsoft Teams Audio device is not installed"
if pgrep -f "Microsoft Teams.app/Contents/MacOS" >/dev/null 2>&1 || pgrep -x "MSTeams" >/dev/null 2>&1; then
    fail "the Microsoft Teams app is running: quit it first"
fi

echo "smoke: $BPS bytes/s, both modems on $DEVICE, work in $WORK"
"$MODEM" -d "$DEVICE" --bps "$BPS" --link "$WORK/a" --monitor --debug 1 >"$WORK/a.log" 2>&1 &
MODEM_A=$!
"$MODEM" -d "$DEVICE" --bps "$BPS" --link "$WORK/b" --monitor --debug 1 >"$WORK/b.log" 2>&1 &
MODEM_B=$!
MODEMS="$MODEM_A $MODEM_B"
wait_for "$WORK/a" || fail "modem A did not start (see $WORK/a.log)"
wait_for "$WORK/b" || fail "modem B did not start (see $WORK/b.log)"

# The computers: each reads its modem's KISS output from the start.
cat "$WORK/a" >"$WORK/a.kiss" &
READERS="$READERS $!"
cat "$WORK/b" >"$WORK/b.kiss" &
READERS="$READERS $!"

# A sends two frames in one write (a shared FEND between them), the second with FEND and FESC in its data (escaped as
# FESC TFEND and FESC TFESC); B must hand its computer the same two frames, escaped the same way.
printf '\300\000Hello from A, frame 1\300\000frame 2: \333\334 and \333\335 inside\300' >"$WORK/a"
printf '\300\000Hello from A, frame 1\300\300\000frame 2: \333\334 and \333\335 inside\300' >"$WORK/b.expected"
wait_bytes "$WORK/b.kiss" "$WORK/b.expected" || fail "B did not hand over A's frames byte for byte"
echo "smoke: A -> B: 2 frames byte for byte"

# B answers; A must hear it. Every frame here is at least 15 bytes: the modems' default --min-frame (V23) drops
# shorter receptions.
printf '\300\000Reply from B, over\300' >"$WORK/b"
printf '\300\000Reply from B, over\300' >"$WORK/a.expected"
wait_bytes "$WORK/a.kiss" "$WORK/a.expected" || fail "A did not hand over B's answer byte for byte"
echo "smoke: B -> A: 1 frame byte for byte"

# A third program keys once with --test-tx (no channel check) while both listen; B must hand it over too.
"$MODEM" -d "$DEVICE" --bps "$BPS" --test-tx "test-tx from C, 73" >"$WORK/c.log" 2>&1 || fail "--test-tx failed (see $WORK/c.log)"
printf '\300\000Hello from A, frame 1\300\300\000frame 2: \333\334 and \333\335 inside\300\300\000test-tx from C, 73\300' >"$WORK/b.expected"
wait_bytes "$WORK/b.kiss" "$WORK/b.expected" || fail "B did not hand over the --test-tx frame byte for byte"
echo "smoke: --test-tx -> B: 1 frame byte for byte"

# A clean stop (SIGINT): each modem exits 0 and removes its link.
kill -INT "$MODEM_A" "$MODEM_B"
wait "$MODEM_A"
status_a=$?
wait "$MODEM_B"
status_b=$?
MODEMS=""
for pid in $READERS; do kill -INT "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; done
READERS=""
[ "$status_a" -eq 0 ] || fail "A exited with $status_a"
[ "$status_b" -eq 0 ] || fail "B exited with $status_b"
[ -e "$WORK/a" ] && fail "A left its link"
[ -e "$WORK/b" ] && fail "B left its link"
echo "smoke: A's monitor and debug:"
sed 's/^/    /' "$WORK/a.log"
echo "smoke: B's monitor and debug:"
sed 's/^/    /' "$WORK/b.log"
grep -q "warning" "$WORK/a.log" "$WORK/b.log" && echo "smoke: note: a warning above"
echo "smoke: PASS: frames byte for byte both ways; both modems stopped with exit 0 and removed their links"
