#pragma once

// The dynamic-peer registry [P4]: peers spawned at runtime via `bus spawn`
// (kilvin, the taro spokes, …) persisted so a fleet-layout relaunch restores
// them — fleet.kdl agents never come through `bus spawn`, so this holds
// exactly the dynamically-spawned set.
//
// One record per peer under $STATE/dynamic-peers/ (filename = peer name),
// atomic add/remove with no rewrite races. This type OWNS that on-disk layout
// — before it, the "/dynamic-peers" path + the role=/project_dir=/profile=
// record format were open-coded at four call sites (three in sub_lifecycle.cpp,
// one in broker.cpp's GC liveness scan). A layout change now touches one file.

#include <string>
#include <vector>

namespace bus {

// A dynamically spawned peer's respawn spec. `profile` MUST persist or a
// restore-peers relaunch silently demotes an orchestrator back to the default
// (worker) envelope. Session continuity is free (agent-launch --continue
// resumes by name+workspace), so no session id is stored.
struct Peer {
  std::string name;
  std::string role;
  std::string project_dir;
  std::string profile;
};

class PeerRegistry {
 public:
  explicit PeerRegistry(std::string state_root);

  // Persist `p` (create-or-overwrite), creating the registry dir on first add.
  // Best-effort: a write failure leaves no record, matching the prior inline
  // behavior. `p.name` is the filename, so it must be a bare peer name.
  auto add(const Peer& p) const -> void;

  // Remove the peer's record. Returns true iff a record existed.
  auto remove(const std::string& name) const -> bool;

  // Every persisted peer (empty when the registry dir is absent). A malformed
  // record still yields its `name` with empty fields, so callers that only
  // need names (broker GC liveness) see every registered peer.
  auto list() const -> std::vector<Peer>;

 private:
  std::string dir_;  // <state_root>/dynamic-peers
};

}  // namespace bus
