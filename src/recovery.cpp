// P2 auto-recovery ledger + breaker/backoff guards. See recovery.h for the
// design rationale (pure, unit-testable, no Loop/socket/real-clock).

#include "recovery.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace bus::delivery {

[[noreturn]] static auto fatal(std::string_view msg) -> void {
  std::string s = "bus::recovery fatal: ";
  s += msg;
  s += '\n';
  [[maybe_unused]] auto _ = ::write(STDERR_FILENO, s.data(), s.size());
  std::abort();
}

auto recoveryModeFromEnv() -> RecoveryMode {
  const char* e = std::getenv("CLAUDE_BUS_AUTO_RECOVERY");
  if (e == nullptr || *e == '\0') return RecoveryMode::Observe;
  const std::string_view v{e};
  if (v == "off" || v == "0") return RecoveryMode::Off;
  if (v == "observe")         return RecoveryMode::Observe;
  if (v == "soft")            return RecoveryMode::Soft;
  if (v == "on")              return RecoveryMode::On;
  fatal(std::format("unrecognized CLAUDE_BUS_AUTO_RECOVERY=\"{}\" "
                    "(valid: off, 0, observe, soft, on)", v));
}

// ── monotonic clock + boot id (§6.1) ──────────────────────────────────────

auto nowMonoMs() -> std::int64_t {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
      .count();
}

auto readBootId() -> std::string {
  std::ifstream f{"/proc/sys/kernel/random/boot_id"};
  std::string id;
  std::getline(f, id);
  return id;  // "" if unreadable → treated as a fresh boot on every load
}

namespace {

auto breakerToStr(BreakerState b) -> std::string {
  switch (b) {
    case BreakerState::Open:
      return "open";
    case BreakerState::HalfOpen:
      return "half-open";
    default:
      return "closed";
  }
}

auto breakerFromStr(std::string_view s) -> BreakerState {
  if (s == "open") return BreakerState::Open;
  if (s == "half-open") return BreakerState::HalfOpen;
  return BreakerState::Closed;
}

}  // namespace

// ── ledger <-> JSON (§7 persistence) ───────────────────────────────────────

auto ledgerToJson(const RecoveryLedger& l) -> json::Value {
  std::map<std::string, json::Value> m;
  m.insert({"boot_id", json::Value::from(l.boot_id)});
  std::map<std::string, json::Value> sigs;
  for (const auto& [k, s] : l.sigs) {
    std::map<std::string, json::Value> so;
    so.insert({"last_fired_mono_ms", json::Value::from(s.last_fired_mono_ms)});
    so.insert(
        {"attempts", json::Value::from(static_cast<std::int64_t>(s.attempts))});
    so.insert(
        {"backoff_until_mono_ms", json::Value::from(s.backoff_until_mono_ms)});
    sigs.insert({k, json::Value::fromObject(std::move(so))});
  }
  m.insert({"sigs", json::Value::fromObject(std::move(sigs))});
  std::vector<json::Value> rl;
  rl.reserve(l.relaunch_mono_ms.size());
  for (auto t : l.relaunch_mono_ms) rl.push_back(json::Value::from(t));
  m.insert({"relaunch_mono_ms", json::Value::fromArray(std::move(rl))});
  m.insert({"breaker", json::Value::from(breakerToStr(l.breaker))});
  m.insert({"open_until_mono_ms", json::Value::from(l.open_until_mono_ms)});
  return json::Value::fromObject(std::move(m));
}

auto ledgerFromJson(const json::Value& v) -> RecoveryLedger {
  RecoveryLedger l;
  l.boot_id = v.getOrString("boot_id");
  if (const auto* sigs = v.get("sigs"); sigs != nullptr && sigs->isObject()) {
    for (const auto& [k, sv] : sigs->asObject()) {
      RecoverySig s;
      s.last_fired_mono_ms = sv.getOrInt("last_fired_mono_ms", 0);
      s.attempts = static_cast<std::int32_t>(sv.getOrInt("attempts", 0));
      s.backoff_until_mono_ms = sv.getOrInt("backoff_until_mono_ms", 0);
      l.sigs.insert({k, s});
    }
  }
  if (const auto* rl = v.get("relaunch_mono_ms");
      rl != nullptr && rl->isArray()) {
    for (const auto& t : rl->asArray()) l.relaunch_mono_ms.push_back(t.asInt());
  }
  l.breaker = breakerFromStr(v.getOrString("breaker", "closed"));
  l.open_until_mono_ms = v.getOrInt("open_until_mono_ms", 0);
  return l;
}

// ── breaker/backoff guards (§6) — pure functions over the ledger ───────────

auto recoveryThresholds() -> RecoveryThresholds {
  RecoveryThresholds th;
  auto envI = [](const char* k, std::int64_t& dst) {
    if (const char* e = std::getenv(k); e != nullptr && *e != '\0') {
      if (const auto v = std::atoll(e); v > 0) dst = v;
    }
  };
  envI("CLAUDE_BUS_RECOVERY_BACKOFF_BASE_MS", th.backoff_base_ms);
  envI("CLAUDE_BUS_RECOVERY_BACKOFF_CAP_MS", th.backoff_cap_ms);
  envI("CLAUDE_BUS_RECOVERY_MAXT_MS", th.maxt_ms);
  std::int64_t maxr = th.maxr;
  envI("CLAUDE_BUS_RECOVERY_MAXR", maxr);
  th.maxr = static_cast<std::int32_t>(maxr);
  envI("CLAUDE_BUS_RECOVERY_BREAKER_COOLDOWN_MS", th.breaker_cooldown_ms);
  return th;
}

auto recoveryDecide(const RecoveryLedger& l, const std::string& sig,
                    RecoveryAction action, std::int64_t now_mono,
                    const RecoveryThresholds& th) -> GuardDecision {
  (void)th;
  // Per-signature exponential backoff gates EVERY action (anti-flap).
  if (auto it = l.sigs.find(sig); it != l.sigs.end()) {
    if (now_mono < it->second.backoff_until_mono_ms) return {false, "backoff"};
  }
  // Only Relaunch (destructive) is breaker-gated; nudge/clear ride backoff
  // alone — §6 "non-destructive may fire on one strong signal".
  if (action != RecoveryAction::Relaunch) return {true, {}};
  switch (l.breaker) {
    case BreakerState::Closed:
      return {true, {}};
    case BreakerState::Open:
      // Cooldown elapsed → one half-open probe is eligible.
      if (now_mono >= l.open_until_mono_ms) return {true, {}};
      return {false, "breaker-open"};
    case BreakerState::HalfOpen:
      // A probe already fired; throttle to one relaunch per cooldown until the
      // agent recovers (recoveryObserveHealthy closes it). Same backoff shape
      // as Open — "failure re-opens" == stays in the probe-rate loop.
      if (now_mono >= l.open_until_mono_ms) return {true, {}};
      return {false, "breaker-half-open"};
  }
  return {false, "unknown"};
}

auto recoveryRecord(RecoveryLedger& l, const std::string& sig,
                    RecoveryAction action, std::int64_t now_mono,
                    const RecoveryThresholds& th) -> BreakerState {
  // Arm per-signature backoff: base × 2^attempts, capped. attempts counts
  // prior fires, so the first fire backs off base, the next 2×base, etc.
  auto& s = l.sigs[sig];
  s.last_fired_mono_ms = now_mono;
  const std::int32_t shift = std::min(s.attempts, 16);  // cap the shift
  std::int64_t backoff = th.backoff_base_ms << shift;
  if (backoff > th.backoff_cap_ms || backoff < 0) backoff = th.backoff_cap_ms;
  s.backoff_until_mono_ms = now_mono + backoff;
  s.attempts += 1;

  if (action != RecoveryAction::Relaunch) return l.breaker;

  // A relaunch fired while Open/HalfOpen: this IS the half-open probe. Stay
  // HalfOpen and re-arm the cooldown so the next probe waits a full window.
  if (l.breaker == BreakerState::Open || l.breaker == BreakerState::HalfOpen) {
    l.breaker = BreakerState::HalfOpen;
    l.open_until_mono_ms = now_mono + th.breaker_cooldown_ms;
    return l.breaker;
  }

  // Closed: record the relaunch in the MaxR/MaxT window, prune stale entries,
  // trip the breaker on overflow.
  l.relaunch_mono_ms.push_back(now_mono);
  std::erase_if(l.relaunch_mono_ms,
                [&](std::int64_t t) { return now_mono - t > th.maxt_ms; });
  if (static_cast<std::int32_t>(l.relaunch_mono_ms.size()) >= th.maxr) {
    l.breaker = BreakerState::Open;
    l.open_until_mono_ms = now_mono + th.breaker_cooldown_ms;
  }
  return l.breaker;
}

auto recoveryObserveHealthy(RecoveryLedger& l, std::int64_t now_mono,
                            const RecoveryThresholds& th) -> void {
  (void)now_mono;
  (void)th;
  // A half-open probe succeeded (the agent matched no destructive signature
  // this scan): close the breaker, clear the thrash window, and reset
  // per-signature backoff so a fresh future failure starts from base. Only
  // the HalfOpen→Closed edge resets — a Closed agent oscillating
  // healthy/unhealthy must NOT have its backoff wiped each scan (that would
  // defeat backoff entirely), and an Open breaker stays tripped until its
  // probe actually fires and succeeds.
  if (l.breaker == BreakerState::HalfOpen) {
    l.breaker = BreakerState::Closed;
    l.open_until_mono_ms = 0;
    l.relaunch_mono_ms.clear();
    for (auto& [k, s] : l.sigs) {
      (void)k;
      s.attempts = 0;
      s.backoff_until_mono_ms = 0;
    }
  }
}

}  // namespace bus::delivery
