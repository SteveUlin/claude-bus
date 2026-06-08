#pragma once

// Context-window + effort reader: derives an agent's CTX% / window / model /
// effort from its on-disk capture. A pure Readers-family derivation (no
// authority, reconstructs from files), lifted out of sub_monitor.cpp's render
// layer so any viewer — not just the monitor — can read it.
//
// The state root is INJECTED (the Readers convention: no global acquisition),
// which also makes it unit-testable against a temp dir.

#include <string>
#include <string_view>

namespace bus {

struct CtxStats {
  int pct{-1};                // used_percentage against the REAL window
  long long size_tokens{-1};  // REAL per-model window (the honest denominator)
  long long tokens{-1};       // total_input_tokens — drives the policy marker
  std::string model;          // model id ("" if absent)
  std::string effort;         // live effort level; "" if unavailable (gap)
};

// Context + effort for `agent` under `state_root`. AUTHORITATIVE source is the
// statusline capture (<state_root>/statusline/<agent>.json — real per-model
// window + live effort, the only place those exist; see
// settings/hooks-shared/statusline.sh). Falls back to the broker's
// transcript-derived <state_root>/status (knob window, no effort) when the
// capture is absent — e.g. before the wrapper has rolled out to a pane.
auto contextStatsFor(std::string_view state_root, std::string_view agent)
    -> CtxStats;

}  // namespace bus
