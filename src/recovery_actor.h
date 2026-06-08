#pragma once

// RecoveryActor — the P2 auto-recovery engine as a Policy actor: the reference
// actor proving the Policy seam (docs/policy-actors.md §3). Its evaluate() is
// the former Loop::maybeAutoRecover body verbatim — the wall-jump grace, the
// R1-R4 signature table, the recoveryDecide / recoveryRecord guards (pure, in
// recovery.h) — owning the per-agent ledger + cooldown + clock-jump state that
// used to be loose Loop members. It emits Enqueue actions (would-recover audit
// rows; in soft/on mode the R1 /clear) the loop executes. Behavior is identical
// to the inline engine — the extraction is a move, deploy-verified against the
// live observe audit stream.
//
// The observe/soft/on activation flag is a CONSTRUCTOR arg (CLAUDE_BUS_AUTO_
// RECOVERY) — a runtime decision about whether emitted actions are real or
// logged-only, which the PolicyEngine never sees. Orthogonal to the seam.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "policy.h"
#include "recovery.h"

namespace bus::delivery {

class RecoveryActor : public policy::PolicyActor {
 public:
  // state_dir backs the ledger ($STATE/recovery/<agent>.json); mode is the
  // resolved CLAUDE_BUS_AUTO_RECOVERY value. Loads the ledger on construction
  // (a boot-id mismatch resets the windows).
  RecoveryActor(std::string state_dir, RecoveryMode mode);

  auto name() const -> std::string_view override { return "recovery"; }
  auto evaluate(const policy::PolicyContext& ctx)
      -> std::vector<policy::PolicyAction> override;
  // Drop would-recover cooldown entries for agents no longer live (the ledger
  // itself persists — a despawned-then-respawned agent keeps its breaker).
  auto pruneDeadAgents(const std::set<std::string>& live) -> void override;

 private:
  std::string state_dir_;
  RecoveryMode mode_{RecoveryMode::Observe};

  // Recovery ledger (per agent), loaded on boot, persisted on mutation.
  std::map<std::string, RecoveryLedger> recovery_;
  // Scan rate-limit + per-(agent,signature) would-recover cooldown so a
  // persistently-wedged agent logs once, not every scan.
  std::int64_t auto_recover_last_scan_ms_{0};
  std::map<std::string, std::int64_t> would_recover_next_log_ms_;
  // Wall-jump detection (§6.1a): the previous tick's (wall,mono) pair; a
  // Δwall − Δmono past the threshold marks a suspend/clock-jump → arm grace.
  std::int64_t last_tick_wall_ms_{0};
  std::int64_t last_tick_mono_ms_{0};
  std::int64_t suspend_grace_until_mono_ms_{0};

  // Recovery-ledger persistence ($STATE/recovery/<agent>.json). loadRecovery
  // runs on construction; saveRecovery after any breaker/backoff mutation. On
  // load, a boot_id mismatch (reboot) resets the agent's monotonic windows.
  auto loadRecovery() -> void;
  auto saveRecovery(const std::string& agent) -> void;
};

}  // namespace bus::delivery
