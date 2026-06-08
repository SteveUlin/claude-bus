#include "recovery_actor.h"

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <set>
#include <sstream>
#include <utility>

#include "agent_status.h"
#include "json_min.h"

namespace bus::delivery {

RecoveryActor::RecoveryActor(std::string state_dir, RecoveryMode mode)
    : state_dir_{std::move(state_dir)}, mode_{mode} {
  loadRecovery();
}

// P2 auto-recovery triage. Evaluates the recovery signature table for every
// live agent and, in observe mode, logs what it WOULD do ("would-recover ...")
// to the audit topic, taking no action; in soft/on mode the R1 idle-clear
// fires through the breaker/backoff ledger. The body is the former
// Loop::maybeAutoRecover verbatim — see docs/broker-auto-recovery.md and §3 of
// docs/policy-actors.md. Inputs (clocks, the per-agent snapshot) arrive via
// ctx; the loop executes the returned Enqueue actions.
auto RecoveryActor::evaluate(const policy::PolicyContext& ctx)
    -> std::vector<policy::PolicyAction> {
  std::vector<policy::PolicyAction> out;

  if (mode_ == RecoveryMode::Off) return out;
  // Soft enables the non-destructive rows (R1 clear); On additionally arms
  // relaunch (Phase C). Observe only logs would-recover and leaves the
  // standalone loops (maybeAutoClear / doorbell) as the actors — so the default
  // config has ZERO behavior change.
  const bool acting = (mode_ == RecoveryMode::Soft || mode_ == RecoveryMode::On);
  const auto rec_th = recoveryThresholds();

  const auto now = ctx.now_wall_ms;
  if (now - auto_recover_last_scan_ms_ < 30'000) return out;  // every 30 s
  auto_recover_last_scan_ms_ = now;

  // §6.1a WALL-JUMP GRACE. Event ages are wall-clock (hooks wall-stamp
  // events.jsonl), so a suspend/resume / NTP step / restart-across-reboot
  // inflates every age → false STUCK/idle. steady_clock PAUSES across suspend
  // while wall LEAPS, so Δwall − Δmono between scans is the jump signature. On
  // a jump, arm a grace window during which the engine NO-OPS: every age
  // spanning the jump is untrustworthy until agents emit fresh post-resume
  // events.
  const auto mono = ctx.now_mono_ms;
  std::int64_t jump_threshold_ms = 5'000;
  if (const char* e = std::getenv("CLAUDE_BUS_CLOCK_JUMP_MS");
      e != nullptr && *e != '\0') {
    if (const auto v = std::atoll(e); v > 0) jump_threshold_ms = v;
  }
  std::int64_t grace_ms = 60'000;
  if (const char* e = std::getenv("CLAUDE_BUS_SUSPEND_GRACE_MS");
      e != nullptr && *e != '\0') {
    if (const auto v = std::atoll(e); v > 0) grace_ms = v;
  }
  if (last_tick_wall_ms_ > 0) {
    const auto d_wall = now - last_tick_wall_ms_;
    const auto d_mono = mono - last_tick_mono_ms_;
    if (d_wall - d_mono > jump_threshold_ms) {
      suspend_grace_until_mono_ms_ = mono + grace_ms;
      std::println(stderr,
                   "recovery: clock jump (d_wall={}ms d_mono={}ms) — "
                   "suspending auto-recovery for {}ms",
                   d_wall, d_mono, grace_ms);
    }
  }
  last_tick_wall_ms_ = now;
  last_tick_mono_ms_ = mono;
  if (mono < suspend_grace_until_mono_ms_) return out;  // in grace → no action

  // Budgets (env-tunable; reuse the escalation budget where it matches).
  std::int64_t stuck_budget = 5 * 60'000;
  if (const char* e = std::getenv("CLAUDE_BUS_STUCK_BUDGET_MS");
      e != nullptr && *e != '\0') {
    if (const auto v = std::atoll(e); v > 0) stuck_budget = v;
  }
  std::int64_t wedge_budget = 5 * 60'000;  // transcript-stale threshold
  if (const char* e = std::getenv("CLAUDE_BUS_WEDGE_BUDGET_MS");
      e != nullptr && *e != '\0') {
    if (const auto v = std::atoll(e); v > 0) wedge_budget = v;
  }
  std::int64_t idle_clear_ms = 10 * 60'000;
  if (const char* e = std::getenv("CLAUDE_BUS_AUTO_CLEAR_MIN");
      e != nullptr && *e != '\0') {
    if (const auto v = std::atoll(e); v > 0) idle_clear_ms = v * 60'000;
  }

  // would-recover audit logger with a per-(agent,signature) cooldown so a
  // persistently-wedged agent logs once, not every scan. Sets the cooldown
  // only when it actually logs. Emits an Enqueue the loop executes.
  auto logWould = [&](const std::string& agent, std::string_view sig,
                      std::string_view action, const std::string& signals) {
    const std::string key = agent + "\x1f" + std::string{sig};
    if (now < would_recover_next_log_ms_[key]) return;
    would_recover_next_log_ms_[key] = now + 5 * 60'000;  // per-sig cooldown
    policy::PolicyAction a;
    a.kind = policy::PolicyAction::Kind::Enqueue;
    a.agent = agent;
    a.topic = "audit";
    a.protocol = "would-recover";
    a.body = std::format(
        "would-recover agent={} signature={} action={} signals=[{}]", agent,
        sig, action, signals);
    out.push_back(std::move(a));
  };

  // R1 ACT (soft/on mode): the engine OWNS idle-context clearing, superseding
  // maybeAutoClear (which steps aside at mode≥soft). Gate on the ledger's
  // per-signature backoff, emit /clear to commands-<agent> exactly as
  // maybeAutoClear did, then record the fire so backoff arms + persists. Uses
  // the MONOTONIC clock (the ledger's clock). Returns true if it cleared.
  auto recoverClear = [&](const std::string& agent,
                          const std::string& signals) -> bool {
    auto& led = recovery_[agent];
    if (led.boot_id.empty()) led.boot_id = readBootId();
    if (!recoveryDecide(led, "idle-context", RecoveryAction::Clear, mono,
                        rec_th)
             .allow) {
      return false;  // backoff — too soon since the last clear
    }
    policy::PolicyAction clear;
    clear.kind = policy::PolicyAction::Kind::Enqueue;
    clear.agent = agent;
    clear.topic = "commands-" + agent;
    clear.body = "/clear";
    clear.protocol = "auto-recover-clear";
    clear.deliver_when = 1;  // idle
    out.push_back(std::move(clear));
    recoveryRecord(led, "idle-context", RecoveryAction::Clear, mono, rec_th);
    saveRecovery(agent);
    // Audit the acted recovery (protocol=recover) — the same place the human
    // sees delivery failures + would-recover rows.
    policy::PolicyAction au;
    au.kind = policy::PolicyAction::Kind::Enqueue;
    au.agent = agent;
    au.topic = "audit";
    au.protocol = "recover";
    au.body = std::format(
        "recover agent={} signature=idle-context action=clear signals=[{}]",
        agent, signals);
    out.push_back(std::move(au));
    return true;
  };

  for (const auto& s : ctx.agents) {
    const std::string& name = s.name;
    const AgentInfo& info = *s.info;
    // Live pane only (loop-filtered into ctx.agents); defer if the human is
    // attached or the agent is mid blocking-op.
    if (s.attached) continue;
    if (s.blocking_op) continue;

    const bool pending = s.inbox_pending;
    const bool has_in_flight = s.has_in_flight;
    const auto ax = s.axes;

    // Transcript-staleness (computed by the loop plane; -1 = unknown).
    const std::int64_t transcript_age = s.transcript_age_ms;
    const bool transcript_stale =
        transcript_age >= 0 && transcript_age > wedge_budget;

    // R1 idle-context → clear. comms/primary are excluded exactly as
    // maybeAutoClear excluded them (high cross-thread continuity — clear only
    // on human cue; see clear-policy.md §6). In observe mode this logs what it
    // WOULD do (maybeAutoClear still acts); in soft/on mode the engine acts
    // through the breaker/backoff ledger and maybeAutoClear steps aside.
    static const std::set<std::string> kClearSkip{"comms", "primary"};
    if (ax.turn == TurnAxis::Ready && !pending && !has_in_flight &&
        info.last.event == "Stop" &&
        (now - info.last.ts_ms) >= idle_clear_ms &&
        !kClearSkip.contains(name)) {
      const std::string signals =
          std::format("idle_min={},inbox=empty,in_flight=0",
                      (now - info.last.ts_ms) / 60'000);
      if (!acting || !recoverClear(name, signals)) {
        // observe mode, or backoff blocked the act → log the intent.
        logWould(name, "idle-context", "clear", signals);
      }
    }

    // R2 relaunch-idle → nudge: at a ready prompt with queued mail. This is the
    // DELIVERY doorbell's exact condition (maybeWakeIdleOffTty), which already
    // acts and is NOT mode-gated — so the engine only LOGS here (acting would
    // double-wake). The doorbell owns this action; the row stays for a whole
    // would-recover stream.
    if (ax.turn == TurnAxis::Ready && pending) {
      logWould(name, "relaunch-idle", "nudge",
               std::format("turn=ready,mail=pending,in_flight={}",
                           has_in_flight ? 1 : 0));
    }

    // R3 hung-turn → nudge: an OPEN turn past 2x budget with no transcript
    // progress (fork-free — event + transcript signals only). OBSERVE-ONLY even
    // in soft mode: a nudge here injects into a MID-TURN pane, which is the
    // mid-stream-dropped-turn failure. Escalation + relaunch (Phase C) are the
    // safe responses to a hung turn, not a nudge — so this row logs intent and
    // never acts.
    if (info.turn_start_ms > 0 &&
        (now - info.turn_start_ms) > 2 * stuck_budget && transcript_stale) {
      logWould(name, "hung-turn", "nudge",
               std::format("turn_open_ms={},transcript_stale_ms={}",
                           now - info.turn_start_ms, transcript_age));
    }

    // R4 — stale + not-input-ready, ROUTED BY SHAPE (recovery owner's call on
    // kvothe's reporting-truth turn-split seam). Cheap event-only pre-filter
    // (idle agents at a ready prompt excluded WITHOUT a fork), then the pane
    // is forked once and the second signal (pane NOT input-ready) must agree —
    // the cardinal two-signals rule; never on transcript-staleness alone. The
    // pane-not-input-ready gate spares parked agents in BOTH branches.
    //   - Quiet (stale, NOTHING in flight — a dropped / silent turn, the
    //     mid-stream-dropped-turn failure) → NUDGE: a re-prompt resumes it;
    //     relaunch would needlessly nuke recoverable context. signature
    //     "dropped-turn".
    //   - Working / Stuck (a tool call OVERDUE) / process=Stuck (boot hang) —
    //     a genuine wedge → RELAUNCH (the heavy 2-signal action). signature
    //     "wedged".
    // turn is single-valued and Quiet implies process=Alive, so the two
    // branches are mutually exclusive.
    const bool wedged = ax.turn == TurnAxis::Working ||
                        ax.turn == TurnAxis::Stuck ||
                        ax.process == ProcessAxis::Stuck;
    const bool dropped_turn = ax.turn == TurnAxis::Quiet;
    if (transcript_stale && (wedged || dropped_turn)) {
      const std::string sig = dropped_turn ? "dropped-turn" : "wedged";
      const std::string action = dropped_turn ? "nudge" : "relaunch";
      const std::string wkey = name + "\x1f" + sig;
      if (now >= would_recover_next_log_ms_[wkey]) {
        const auto pane = ctx.pane(name);  // forks zellij — gated above
        const auto ax_pane =
            computeAxes(info, pending ? 1 : 0, now, true, &pane);
        const bool awaiting_input = wakeReadyForMail(ax_pane, &pane) ||
                                    ax_pane.turn == TurnAxis::NeedsInput;
        if (!awaiting_input) {
          logWould(name, sig, action,
                   std::format("transcript_stale_ms={},turn={},"
                               "pane_awaiting_input=false",
                               transcript_age,
                               dropped_turn ? "quiet" : "busy"));
        }
      }
    }

    // R5 thinking-block (#10) needs an error-signature source (TBD); R6
    // context-100% needs P5 output-verification. Both are documented
    // placeholders, not yet evaluated. See docs/broker-auto-recovery.md.
  }

  return out;
}

auto RecoveryActor::pruneDeadAgents(const std::set<std::string>& live) -> void {
  // would_recover_next_log_ms_ is keyed by "<agent>\x1f<sig>" — drop every
  // signature entry for an agent no longer in the live set.
  std::erase_if(would_recover_next_log_ms_, [&](const auto& kv) {
    const auto sep = kv.first.find('\x1f');
    const auto agent =
        sep == std::string::npos ? kv.first : kv.first.substr(0, sep);
    return !live.contains(agent);
  });
}

auto RecoveryActor::loadRecovery() -> void {
  namespace fs = std::filesystem;
  const std::string dir = state_dir_ + "/recovery";
  const std::string boot = readBootId();
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (e.path().extension() != ".json") continue;
    const auto agent = e.path().stem().string();
    std::ifstream f{e.path()};
    std::stringstream ss;
    ss << f.rdbuf();
    auto parsed = json::parse(ss.str());
    if (!parsed) continue;
    auto l = ledgerFromJson(*parsed);
    if (l.boot_id != boot) {
      l = RecoveryLedger{};  // reboot → windows invalid; reset
      l.boot_id = boot;
    }
    recovery_.insert_or_assign(agent, std::move(l));
  }
}

// Persist one agent's ledger atomically (tmp + rename). Called after any
// breaker/backoff/relaunch-window mutation so the breaker survives a restart.
auto RecoveryActor::saveRecovery(const std::string& agent) -> void {
  namespace fs = std::filesystem;
  auto it = recovery_.find(agent);
  if (it == recovery_.end()) return;
  if (it->second.boot_id.empty()) it->second.boot_id = readBootId();
  const std::string dir = state_dir_ + "/recovery";
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string path = dir + "/" + agent + ".json";
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f{tmp, std::ios::trunc};
    f << json::serialize(ledgerToJson(it->second));
  }
  fs::rename(tmp, path, ec);
}

}  // namespace bus::delivery
