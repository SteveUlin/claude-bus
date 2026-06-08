#include "peer_registry.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace bus {

namespace fs = std::filesystem;

PeerRegistry::PeerRegistry(std::string state_root)
    : dir_{std::move(state_root) + "/dynamic-peers"} {}

auto PeerRegistry::add(const Peer& p) const -> void {
  std::error_code ec;
  fs::create_directories(dir_, ec);
  std::ofstream out{dir_ + "/" + p.name};
  if (out) {
    out << "role=" << p.role << "\n"
        << "project_dir=" << p.project_dir << "\n"
        << "profile=" << p.profile << "\n";
  }
}

auto PeerRegistry::remove(const std::string& name) const -> bool {
  std::error_code ec;
  return fs::remove(dir_ + "/" + name, ec);
}

auto PeerRegistry::list() const -> std::vector<Peer> {
  std::vector<Peer> peers;
  std::error_code ec;
  if (!fs::exists(dir_, ec)) return peers;
  for (const auto& entry : fs::directory_iterator(dir_, ec)) {
    if (!entry.is_regular_file()) continue;
    Peer p;
    p.name = entry.path().filename().string();
    std::ifstream in{entry.path()};
    std::string line;
    while (std::getline(in, line)) {
      if (line.starts_with("role=")) {
        p.role = line.substr(5);
      } else if (line.starts_with("project_dir=")) {
        p.project_dir = line.substr(12);
      } else if (line.starts_with("profile=")) {
        p.profile = line.substr(8);
      }
    }
    peers.push_back(std::move(p));
  }
  return peers;
}

}  // namespace bus
