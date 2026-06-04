#!/usr/bin/env bash
# recover-itest.sh — `bus recover <agent>` (docs/bus-recover.md §8). Exercises
# the verb against an ISOLATED $STATE with a fake "claude --name X" process (a
# bash loop whose argv carries the pattern so pgrep -f / the pidfile path both
# resolve it). The verb only KILLS — the fleet.kdl respawn-loop relaunches in a
# real fleet — so "verified" is simulated by appending a fresh events.jsonl line
# for the agent (the respawn's SessionStart), exactly the boundary the verb
# polls past. No broker / zellij needed.
#
# Cases: usage(2) · refuse-broker(2) · attached-defer(30) · kill+verify(0) ·
#        idempotent-already-down(0) · boot-stuck verify-timeout(10) ·
#        concurrent flock (both complete, single kill).

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUS="$ROOT/bin/bus"
[ -x "$BUS" ] || { echo "recover-itest: $BUS not built"; exit 1; }

T="$(mktemp -d /tmp/recover-itest.XXXXXX)"
export CLAUDE_BUS_STATE="$T"
mkdir -p "$T/recovery" "$T/pids" "$T/presence"
fail=0
ck() { if [ "$1" = "$2" ]; then echo "  ok: $3"; else
  echo "  FAIL: $3 (got [$1] want [$2])"; fail=1; fi; }

# Start a fake claude for agent $1; echoes its PID + writes the pidfile.
fakes=()
fake_claude() {
  bash -c 'while :; do sleep 3600; done' "claude --name $1 (fake-itest)" \
    >/dev/null 2>&1 &
  local p=$!
  fakes+=("$p")
  printf '%s\n' "$p" > "$T/pids/$1"
  echo "$p"
}
cleanup() { for p in "${fakes[@]:-}"; do kill "$p" 2>/dev/null; done
  pkill -f 'while :; do sleep 3600' 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT

emit_event() {  # append a fresh events.jsonl line for agent $1
  printf '%s\n' "{\"ts\":\"2026-06-04T00:00:00Z\",\"agent\":\"$1\",\"event\":\"SessionStart\"}" \
    >> "$T/events.jsonl"
}

echo "1. usage / refused target → exit 2"
"$BUS" recover >/dev/null 2>&1; ck "$?" "2" "no args → 2"
"$BUS" recover broker >/dev/null 2>&1; ck "$?" "2" "refuse broker pane → 2"

echo "2. attached-pane defer → exit 30, no kill"
fp=$(fake_claude alice); touch "$T/presence/alice"
"$BUS" recover alice >/dev/null 2>&1; rc=$?
ck "$rc" "30" "presence file → 30"
ck "$(kill -0 "$fp" 2>/dev/null && echo alive || echo dead)" "alive" \
   "attached defer did NOT kill the agent"
rm -f "$T/presence/alice"; kill "$fp" 2>/dev/null

echo "3. kill + verify → exit 0, original PID dead, verified:true"
fp=$(fake_claude bob)
( sleep 0.5; emit_event bob ) &              # simulate the respawn's first event
out=$("$BUS" recover bob --timeout-ms 4000 2>/dev/null); rc=$?
ck "$rc" "0" "kill+verify → 0"
case "$out" in *'"verified":true'*) ck yes yes "JSON verified:true";;
  *) ck "$out" '"verified":true' "JSON verified:true";; esac
case "$out" in *"\"killed_pid\":$fp"*) ck yes yes "JSON killed_pid = the fake PID";;
  *) ck "$out" "killed_pid:$fp" "JSON killed_pid = the fake PID";; esac
ck "$(kill -0 "$fp" 2>/dev/null && echo alive || echo dead)" "dead" \
   "the wedged claude was killed"
wait

echo "4. idempotent on already-down → exit 0"
echo 999999 > "$T/pids/carol"                # stale/dead PID, no live process
( sleep 0.4; emit_event carol ) &
"$BUS" recover carol --timeout-ms 3000 >/dev/null 2>&1
ck "$?" "0" "already-down still verifies → 0"
wait

echo "5. boot-stuck (process up, no event) → exit 10"
fp=$(fake_claude dave)                        # the wedged one (pidfile)
# a SECOND fake comes up after the kill = the boot-stuck respawn; emits NO event
( sleep 0.5; bash -c 'while :; do sleep 3600; done' \
    "claude --name dave (respawn)" >/dev/null 2>&1 & echo $! > "$T/dave2.pid" ) &
out=$("$BUS" recover dave --timeout-ms 2500 2>/dev/null); rc=$?
ck "$rc" "10" "process up + no event → 10 (boot-stuck)"
case "$out" in *'"relaunched":true'*) ck yes yes "JSON relaunched:true";;
  *) ck "$out" '"relaunched":true' "JSON relaunched:true";; esac
wait; [ -f "$T/dave2.pid" ] && { kill "$(cat "$T/dave2.pid")" 2>/dev/null; }

echo "6. concurrent fire → flock serializes, both complete"
fp=$(fake_claude erin)
( sleep 0.6; emit_event erin ) &
"$BUS" recover erin --timeout-ms 4000 >/dev/null 2>&1 & r1=$!
"$BUS" recover erin --timeout-ms 4000 >/dev/null 2>&1 & r2=$!
wait $r1; e1=$?; wait $r2; e2=$?
# Both must complete with a defined recovery exit (0/10/20) — the flock means
# they ran one-after-another, not a double-kill crash.
ok_codes() { case "$1" in 0|10|20) return 0;; *) return 1;; esac; }
if ok_codes "$e1" && ok_codes "$e2"; then ck ok ok "both concurrent recovers completed cleanly (e1=$e1 e2=$e2)"
else ck "e1=$e1,e2=$e2" "both 0/10/20" "concurrent recovers completed cleanly"; fi
wait

echo
if [ "$fail" = 0 ]; then
  echo -e "\033[1mRECOVER OK: kill / verify / defer / idempotent / boot-stuck / flock all hold\033[0m"
else
  echo -e "\033[1mRECOVER FAILED\033[0m"
fi
exit "$fail"
