#include "harness.h"
#include "peer_registry.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace bus;

namespace {
auto makeStateRoot(const char* prefix) -> std::string {
  static int n = 0;
  const std::string sr = std::string{prefix} + std::to_string(++n);
  std::filesystem::remove_all(sr);
  return sr;  // PeerRegistry::add creates the dynamic-peers dir on demand.
}
}  // namespace

// add → list roundtrip preserves every field (profile MUST survive — a restore
// relaunch reads it to keep an orchestrator off the worker envelope).
TEST(peer_registry_add_list_roundtrip) {
  const auto sr = makeStateRoot("/tmp/claude-bus-test-pr-rt-");
  PeerRegistry reg{sr};
  reg.add(Peer{.name = "kilvin",
               .role = "scholar",
               .project_dir = "/home/sulin/taro",
               .profile = "orchestrator"});
  const auto peers = reg.list();
  CHECK_EQ(peers.size(), 1u);
  CHECK_EQ(peers[0].name, std::string{"kilvin"});
  CHECK_EQ(peers[0].role, std::string{"scholar"});
  CHECK_EQ(peers[0].project_dir, std::string{"/home/sulin/taro"});
  CHECK_EQ(peers[0].profile, std::string{"orchestrator"});
  std::filesystem::remove_all(sr);
}

// list() on an absent registry dir is empty, not an error.
TEST(peer_registry_list_absent_is_empty) {
  const auto sr = makeStateRoot("/tmp/claude-bus-test-pr-absent-");
  CHECK_EQ(PeerRegistry{sr}.list().size(), 0u);
}

// remove reports whether a record existed and actually drops it from list().
TEST(peer_registry_remove_reports_and_drops) {
  const auto sr = makeStateRoot("/tmp/claude-bus-test-pr-rm-");
  PeerRegistry reg{sr};
  reg.add(Peer{.name = "spoke", .role = "worker", .project_dir = "", .profile = ""});
  CHECK_EQ(reg.list().size(), 1u);
  CHECK(reg.remove("spoke"));         // existed → true
  CHECK_EQ(reg.list().size(), 0u);
  CHECK(!reg.remove("spoke"));        // already gone → false
  CHECK(!reg.remove("never-here"));   // never existed → false
  std::filesystem::remove_all(sr);
}

// A malformed record still yields its name (broker GC liveness needs every
// registered name even if the body is junk).
TEST(peer_registry_malformed_record_keeps_name) {
  const auto sr = makeStateRoot("/tmp/claude-bus-test-pr-bad-");
  std::filesystem::create_directories(sr + "/dynamic-peers");
  std::filesystem::permissions(sr + "/dynamic-peers",
                               std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::add);
  { std::ofstream f{sr + "/dynamic-peers/ghost"}; f << "garbage\nno fields\n"; }
  const auto peers = PeerRegistry{sr}.list();
  CHECK_EQ(peers.size(), 1u);
  CHECK_EQ(peers[0].name, std::string{"ghost"});
  CHECK_EQ(peers[0].role, std::string{""});
  std::filesystem::remove_all(sr);
}
