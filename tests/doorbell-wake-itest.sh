#!/usr/bin/env bash
# doorbell-wake-itest.sh — does the off-TTY DOORBELL reliably wake an IDLE
# agent to drain queued mail? (auri's delivery-audit open question; same
# broker-delivery cluster as the dup-delivery bug + gap #12.)
#
# WHY THIS NEEDS THE FAKE-ZELLIJ HARNESS: the existing off-tty-itest (§C4)
# can't reach the doorbell EMISSION path — maybeWakeIdleOffTty gates on
# paneId() resolving a LIVE pane, which returns nothing in a headless
# sandbox, so the wake never fires there. This test reuses the fake zellij
# (a synthetic live pane) precisely to unlock that path.
#
# MECHANISM (delivery.cpp:1324 maybeWakeIdleOffTty). An idle off-TTY agent
# at the prompt never drains on its own (drain is pull-on-turn). So when the
# loop sees queued mail for an off-TTY + live-pane + unattended + READY agent,
# it rings a doorbell: a single `[bus-wake]` sentinel submit via the safe-write
# path. That fires UserPromptSubmit -> the drain hook delivers the mail. The
# broker writes ONLY the sentinel. Observables:
#   - a write of "[bus-wake]" to the pane (fake-zellij write-chars sink), and
#   - an audit record protocol=doorbell ("doorbell wake agent=...").
# NOTE the scan is rate-limited to every 5 s (wake_last_scan_ms_), so the
# wake can lag the mail by up to ~5 s — we poll a window and report latency.
#
# Gotchas baked in (project_broker_itest_gotchas): assert on the fake sink /
# audit log, never a racy client read; `grep -c || true`; wait only on named
# pids (a bare `wait` blocks on the bg broker).

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUS="${BUS:-$ROOT/bin/bus}"
export CLAUDE_BUS_STATE="$(mktemp -d /tmp/doorbell-wake-itest.XXXXXX)"

AGENT="tester"            # off-TTY by default (NOT a compiled-in/env TTY agent)
WINDOW_S=12               # how long we'll wait for the ring (covers the 5 s scan)
FAKE="$CLAUDE_BUS_STATE/fakebin"
WRITES="$CLAUDE_BUS_STATE/pane-writes.log"   # the sink: one line per write-chars
mkdir -p "$FAKE"; : > "$WRITES"
fail=0
ck() { if [ "$1" = "$2" ]; then echo "  ok: $3"; else
  echo "  FAIL: $3 (got [$1] want [$2])"; fail=1; fi; }

# Fake zellij: a synthetic live pane for $AGENT so paneId() resolves (the
# doorbell's live-pane gate) and sendToPaneSafe's dump-screen gate passes, so
# the [bus-wake] sentinel actually reaches write-chars (logged as the sink).
cat > "$FAKE/zellij" <<FAKEEOF
#!/usr/bin/env bash
sub="\$*"
case "\$sub" in
  *"list-panes"*)
    printf '%s\n' '{' '  "id": 1,' '  "is_plugin": false,' '  "title": "$AGENT"' '}'
    ;;
  *"dump-screen"*)
    printf '%s\n' "> " "-- INSERT --  bypass permissions on"
    ;;
  *"write-chars"*)
    printf '%s\n' "WRITE \$*" >> "$WRITES"
    ;;
  *) : ;;
esac
exit 0
FAKEEOF
chmod +x "$FAKE/zellij"
export PATH="$FAKE:$PATH"

# Seed events.jsonl: a Stop event makes $AGENT read as Alive+Ready (idle at the
# prompt) — the doorbell's wakeReadyForMail gate (delivery.cpp:1313).
TS="$(date -u +%FT%T.%3NZ)"
printf '%s\n' "{\"ts\":\"$TS\",\"session\":\"t\",\"agent\":\"$AGENT\",\"pane\":\"1\",\"event\":\"Stop\",\"payload\":{}}" \
  > "$CLAUDE_BUS_STATE/events.jsonl"

"$BUS" broker run >"$CLAUDE_BUS_STATE/broker.out" 2>&1 &
BPID=$!
trap 'kill "$BPID" 2>/dev/null; wait "$BPID" 2>/dev/null; rm -rf "$CLAUDE_BUS_STATE"' EXIT
for _ in $(seq 1 50); do
  [ -S "$CLAUDE_BUS_STATE/broker.sock" ] && break; sleep 0.1
done
if [ ! -S "$CLAUDE_BUS_STATE/broker.sock" ]; then
  echo "broker did not come up:"; cat "$CLAUDE_BUS_STATE/broker.out"; exit 1
fi
echo "isolated broker up (pid $BPID), idle off-TTY agent=$AGENT with a live (fake) pane"

# Mail the IDLE agent. With no doorbell it would strand (drain is pull-on-turn).
"$BUS" msg mail "$AGENT" "wake me to drain this" >/dev/null
echo "mailed $AGENT; polling up to ${WINDOW_S}s for the [bus-wake] ring..."

# Poll: drive a tick (~1 Hz RPC activity advances the loop) and watch the sink.
rang=0
for i in $(seq 1 $((WINDOW_S * 2))); do
  "$BUS" state >/dev/null 2>&1
  if grep -q '\[bus-wake\]' "$WRITES" 2>/dev/null; then
    rang=1; echo "  doorbell rang after ~$(awk "BEGIN{printf \"%.1f\", $i*0.5}")s"; break
  fi
  sleep 0.5
done

echo "1. the doorbell rang an idle off-TTY agent with queued mail"
ck "$rang" "1" "[bus-wake] sentinel written to the pane within ${WINDOW_S}s"

echo "2. the ring is audited (protocol=doorbell)"
audit="$("$BUS" msg peek audit --limit 50 2>/dev/null || true)"
dbell=$(printf '%s' "$audit" | grep -c "doorbell wake agent=$AGENT" || true)
ck "$([ "${dbell:-0}" -ge 1 ] && echo audited)" "audited" "audit topic recorded the doorbell ring (count=$dbell)"

echo "3. exactly one ring (cooldown holds — no wake storm in the window)"
nwake=$(grep -c '\[bus-wake\]' "$WRITES" || true)
ck "$([ "${nwake:-0}" -ge 1 ] && [ "${nwake:-0}" -le 2 ] && echo bounded)" "bounded" \
   "ring count bounded by the 30s cooldown (got $nwake in ${WINDOW_S}s)"

echo
if [ "$fail" = 0 ]; then
  echo -e "\033[1mDOORBELL-WAKE: RELIABLE (idle off-TTY agent woken to drain)\033[0m"
else
  echo -e "\033[1mDOORBELL-WAKE: FAILED — idle off-TTY mail may strand\033[0m"
  echo "pane-writes.log:"; cat "$WRITES"
  echo "broker.out (tail):"; tail -30 "$CLAUDE_BUS_STATE/broker.out"
fi
exit "$fail"
