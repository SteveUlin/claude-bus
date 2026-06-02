// `bus verify [AGENT...]` — the P5 verify-side viewer: claimed-done vs
// artifact-present. Flags claims whose artifact can't be found on disk —
// the FALSE done-claim alarm. This is the failure mode that's repeatedly
// bitten the fleet: an agent reports "SEC-1 landed" / "RNG built" but the
// commit or workspace artifact never actually materialized.
//
// Source of claims: $STATE/done/<agent>.jsonl — durable, agent-authored
// records written by `bus done` (W1). NOT orchestrator-tracked, so the
// signal can't inherit the orchestrator's own /clear / relay failures —
// the very unreliability this viewer exists to catch.
//
// artifact-present check (per claim, dual — present = findable any way):
//   - filesystem: artifact exists as a path (absolute, or relative to the
//     agent's workspace or the repo root).
//   - jj commit:  `jj log -r <artifact>` resolves in the agent's workspace
//     (--ignore-working-copy so we never snapshot/disturb a peer's tree).
//   - test:<name>: not machine-checkable yet (no test harness) → manual.
//
// One-shot by design: the jj / filesystem probes are too expensive for a
// 1-Hz loop, and a one-shot can't silently die mid-frame like a long-lived
// viewer can. Re-run it to refresh.

#include "../agent_status.h"
#include "../json_min.h"
#include "../state_paths.h"
#include "../sub.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <fstream>
#include <print>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bus {

namespace {

using namespace bus::ansi;

// popen wrapper that returns the child's exit status alongside output.
// We only need the status for jj (did the revset resolve?), but capture
// both so a future caller can inspect stdout.
struct CmdResult {
  int status{-1};
  std::string out;
};

auto runCommand(const std::string& cmd) -> CmdResult {
  CmdResult r;
  FILE* pipe = ::popen(cmd.c_str(), "r");
  if (pipe == nullptr) return r;
  std::array<char, 512> buf{};
  while (std::fgets(buf.data(), buf.size(), pipe) != nullptr) {
    r.out.append(buf.data());
  }
  r.status = ::pclose(pipe);
  return r;
}

// Single-quote a string for safe interpolation into a /bin/sh command.
// Artifacts are agent-authored, so treat them as untrusted input.
auto shellQuote(std::string_view s) -> std::string {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += '\'';
  return out;
}

// A completion claim read from $STATE/done/<agent>.jsonl.
struct Claim {
  std::string agent;
  std::string task;
  std::string artifact;
  std::int64_t ts{};
};

// Resolve an agent's workspace path from the registry
// ($STATE/agents/<name>.json). Empty when absent.
auto workspaceFor(std::string_view agent) -> std::string {
  const std::string reg =
      stateRoot() + "/agents/" + std::string{agent} + ".json";
  std::ifstream in{reg};
  if (!in) return {};
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  const auto v = json::parse(content);
  if (!v) return {};
  return v->getOrString("workspace");
}

enum class Kind { Commit, Path, Test, Unknown };
enum class Present { Yes, No, Manual };

auto classifyKind(std::string_view artifact) -> Kind {
  if (artifact.starts_with("test:")) return Kind::Test;
  if (artifact.find('/') != std::string_view::npos) return Kind::Path;
  return Kind::Unknown;  // could be a path or a jj id — probe both
}

// Does the artifact exist as a path, relative to workspace / repo / abs?
auto pathPresent(std::string_view artifact, const std::string& workspace,
                 const std::string& repo) -> bool {
  std::error_code ec;
  if (std::filesystem::exists(std::filesystem::path{artifact}, ec)) return true;
  if (!workspace.empty() &&
      std::filesystem::exists(workspace + "/" + std::string{artifact}, ec))
    return true;
  if (!repo.empty() &&
      std::filesystem::exists(repo + "/" + std::string{artifact}, ec))
    return true;
  return false;
}

// Does the artifact resolve to a jj commit/change in the workspace?
// --ignore-working-copy: never snapshot a peer's tree (see the
// jj-update-stale / shared-repo lessons). 2>/dev/null: an unresolved
// revset is a normal "not a commit" answer, not an error to surface.
auto commitPresent(std::string_view artifact,
                   const std::string& workspace) -> bool {
  if (workspace.empty()) return false;
  const std::string cmd = "jj --ignore-working-copy -R " +
                          shellQuote(workspace) +
                          " log --no-graph -r " + shellQuote(artifact) +
                          " -T commit_id 2>/dev/null";
  const auto r = runCommand(cmd);
  return r.status == 0 && !r.out.empty();
}

auto checkPresent(const Claim& c) -> Present {
  const Kind kind = classifyKind(c.artifact);
  if (kind == Kind::Test) return Present::Manual;

  const std::string workspace = workspaceFor(c.agent);
  const auto ws_pos = workspace.find("/.workspaces/");
  const std::string repo = ws_pos == std::string::npos
                               ? workspace
                               : workspace.substr(0, ws_pos);

  if (pathPresent(c.artifact, workspace, repo)) return Present::Yes;
  if (commitPresent(c.artifact, workspace)) return Present::Yes;
  return Present::No;
}

auto kindLabel(std::string_view artifact) -> std::string_view {
  switch (classifyKind(artifact)) {
    case Kind::Test: return "test";
    case Kind::Path: return "path";
    case Kind::Commit: return "commit";
    case Kind::Unknown: return "ref";
  }
  return "ref";
}

// Read all claims from $STATE/done/, optionally filtered to `only`.
auto readClaims(const std::set<std::string>& only) -> std::vector<Claim> {
  std::vector<Claim> claims;
  const std::string dir = stateRoot() + "/done";
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec)) return claims;
  for (const auto& entry : std::filesystem::directory_iterator{dir, ec}) {
    if (!entry.is_regular_file()) continue;
    if (entry.path().extension() != ".jsonl") continue;
    std::ifstream in{entry.path()};
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty()) continue;
      const auto v = json::parse(line);
      if (!v) continue;
      Claim c;
      c.agent = v->getOrString("agent");
      c.task = v->getOrString("task");
      c.artifact = v->getOrString("claimed_artifact");
      c.ts = v->getOrInt("ts");
      if (!only.empty() && !only.contains(c.agent)) continue;
      claims.push_back(std::move(c));
    }
  }
  return claims;
}

auto truncate(std::string s, std::size_t w) -> std::string {
  if (s.size() <= w) return s;
  s.resize(w - 1);
  s += "…";
  return s;
}

}  // namespace

auto subVerify(std::span<const char* const> args) -> int {
  std::set<std::string> only;
  for (const char* a : args) only.insert(a);

  auto claims = readClaims(only);
  if (claims.empty()) {
    std::println("no completion claims yet — agents stamp them with "
                 "`bus done \"<task>\" \"<artifact>\"`");
    return 0;
  }

  // Stable order: by agent, then by claim time.
  std::sort(claims.begin(), claims.end(), [](const Claim& a, const Claim& b) {
    if (a.agent != b.agent) return a.agent < b.agent;
    return a.ts < b.ts;
  });

  std::println("{}{:<10} {:<11} {:<7} {:<28} {}{}", kBold, "AGENT", "STATUS",
               "KIND", "ARTIFACT", "TASK", kReset);

  int missing = 0;
  for (const auto& c : claims) {
    const Present p = checkPresent(c);
    std::string_view color;
    std::string status;
    switch (p) {
      case Present::Yes:
        color = kGreen;
        status = "✓ present";
        break;
      case Present::No:
        color = kBrightRed;
        status = "✗ MISSING";
        ++missing;
        break;
      case Present::Manual:
        color = kDim;
        status = "? manual";
        break;
    }
    std::println("{:<10} {}{:<11}{} {:<7} {:<28} {}", truncate(c.agent, 10),
                 color, status, kReset, kindLabel(c.artifact),
                 truncate(c.artifact, 28), truncate(c.task, 40));
  }

  std::println("");
  if (missing > 0) {
    std::println("{}{} claim(s) claimed-but-absent — false done-claim "
                 "alarm{}", kBrightRed, missing, kReset);
  } else {
    std::println("{}all claims verified present{}", kGreen, kReset);
  }
  return missing > 0 ? 1 : 0;
}

}  // namespace bus
