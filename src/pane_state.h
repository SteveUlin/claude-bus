#pragma once

// The PaneState VALUE type — split out of pane.h so the Readers seam can
// consume a pane snapshot WITHOUT seeing pane acquisition.
//
// pane.h bundles the actuator/acquisition API (paneState, paneStateCached,
// sendToPaneSafe, the loop-thread TTL cache) with this plain struct. The
// derivation core (agent_status / task_model / trigger_feed / verify) needs
// only the struct: it takes `const PaneState*` as an INJECTED value and never
// fetches one itself (the broker loop acquires the snapshot and passes it in).
//
// By including THIS header instead of pane.h, a reader physically cannot name
// paneState() — the function isn't declared in its translation unit, so a
// stray acquisition call is a COMPILE error, not a runtime surprise. That is
// the broker-seam-redesign §2 "Readers consumes PaneState as a value
// parameter" boundary made structural in the header graph. If a reader ever
// needs to acquire a pane, the seam has failed and should re-split — don't
// reach for pane.h from the read plane.

#include <string>

namespace bus {

// State of a Claude Code TUI pane derived from
// `zellij action dump-screen --ansi`. All fields are best-effort:
// `ok=false` means the dump failed (pane gone, zellij absent, etc.);
// empty strings on individual fields are the wire-level "(empty)"
// sentinel from the pane parser.
struct PaneState {
  bool ok{false};
  std::string mode;          // INSERT / NORMAL / VISUAL / LOCKED / unknown
  std::string buffer;        // typed input; "(empty)" when blank
  std::string suggestion;    // ghost autocomplete; "(none)" when blank
  std::string bypass_perms;  // "on" / "off" / "" (unknown)

  // Typed predicates over the wire-string fields. Keep the raw fields
  // (they're the JSON/RPC wire and the parsing target in pane.cpp);
  // business logic should go through these rather than string literals.
  auto isInsert()    const -> bool { return mode == "INSERT"; }
  auto isLocked()    const -> bool { return mode == "LOCKED"; }
  auto modeKnown()   const -> bool { return mode != "unknown"; }
  auto bypassOn()    const -> bool { return bypass_perms == "on"; }
  auto bypassKnown() const -> bool { return !bypass_perms.empty(); }
  auto hasBuffer()   const -> bool { return buffer != "(empty)"; }
};

}  // namespace bus
