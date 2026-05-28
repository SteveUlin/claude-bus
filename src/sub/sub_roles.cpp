// `bus roles [NAME]` — discoverable agent-role cards.
//
// List mode (no args): print one short row per roles/*.md — the
// frontmatter `name` and `description`. Lets a router decide who to
// dispatch to in one shell call instead of opening + grepping each
// file.
//
// Detail mode (`bus roles NAME`): print the full body of
// roles/NAME.md (frontmatter stripped) — the role's own description
// of territory, in/out-of-scope, etc.
//
// Read-only over roles/*.md. Zero risk. See docs/kvothe-ideation.md
// §2.

#include "../sub.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace bus {

namespace {

// Locate the roles directory. Honors $CLAUDE_BUS_ROOT (set by the
// fleet layout), then falls back to `<bin-dir>/../roles` resolved
// from /proc/self/exe — same shape agent-launch already assumes.
auto rolesDir() -> std::string {
  if (const char* root = std::getenv("CLAUDE_BUS_ROOT")) {
    return std::string{root} + "/roles";
  }
  char buf[4096];
  const auto n = ::readlink("/proc/self/exe", buf, sizeof(buf));
  if (n <= 0) return "/home/sulin/claude-bus/roles";
  std::string path(buf, static_cast<std::size_t>(n));
  const auto bin_slash = path.find_last_of('/');
  if (bin_slash == std::string::npos) return "/home/sulin/claude-bus/roles";
  const auto root_slash = path.find_last_of('/', bin_slash - 1);
  if (root_slash == std::string::npos) return "/home/sulin/claude-bus/roles";
  return path.substr(0, root_slash) + "/roles";
}

struct RoleHeader {
  std::string name;
  std::string description;
};

// Walk the `---\n…\n---\n` YAML-ish frontmatter at the head of a role
// file. Pulls just `name:` and `description:` — anything else is
// ignored. Whitespace-tolerant, no quotes-handling beyond stripping a
// pair of bracketing `"` from the value.
auto parseHeader(std::istream& in) -> RoleHeader {
  RoleHeader h;
  std::string line;
  if (!std::getline(in, line)) return h;
  if (line != "---") return h;  // not a frontmatter file
  while (std::getline(in, line)) {
    if (line == "---") break;
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string key = line.substr(0, colon);
    std::string val = line.substr(colon + 1);
    // strip leading whitespace from val
    std::size_t i = 0;
    while (i < val.size() && (val[i] == ' ' || val[i] == '\t')) ++i;
    val.erase(0, i);
    // strip trailing whitespace
    while (!val.empty() &&
           (val.back() == ' ' || val.back() == '\t' || val.back() == '\r')) {
      val.pop_back();
    }
    // strip quotes if present
    if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
      val = val.substr(1, val.size() - 2);
    }
    if (key == "name") h.name = std::move(val);
    else if (key == "description") h.description = std::move(val);
  }
  return h;
}

// Body = file content after the closing `---` of the frontmatter.
// Returns the whole file if no frontmatter exists.
auto readBody(const std::filesystem::path& p) -> std::string {
  std::ifstream in{p};
  if (!in) return {};
  std::string line;
  if (!std::getline(in, line)) return {};
  if (line != "---") {
    // No frontmatter — rewind and return everything.
    std::ostringstream buf;
    in.seekg(0);
    buf << in.rdbuf();
    return buf.str();
  }
  while (std::getline(in, line)) {
    if (line == "---") break;
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  std::string out = buf.str();
  // Drop leading blank lines from the body for cleaner output.
  std::size_t i = 0;
  while (i < out.size() && (out[i] == '\n' || out[i] == '\r')) ++i;
  if (i > 0) out.erase(0, i);
  return out;
}

auto collectRoles() -> std::vector<std::pair<std::string, RoleHeader>> {
  std::vector<std::pair<std::string, RoleHeader>> out;
  std::error_code ec;
  const auto dir = rolesDir();
  if (!std::filesystem::exists(dir, ec)) return out;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (entry.path().extension() != ".md") continue;
    std::ifstream in{entry.path()};
    if (!in) continue;
    auto h = parseHeader(in);
    if (h.name.empty()) h.name = entry.path().stem().string();
    out.emplace_back(entry.path().stem().string(), std::move(h));
  }
  std::sort(out.begin(), out.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  return out;
}

}  // namespace

auto subRoles(std::span<const char* const> args) -> int {
  if (args.size() > 1) {
    std::println(stderr, "usage: bus roles [NAME]");
    return 2;
  }

  // List mode.
  if (args.empty()) {
    const auto rolls = collectRoles();
    if (rolls.empty()) {
      std::println(stderr, "bus roles: no role files in {}", rolesDir());
      return 1;
    }
    // Compute the widest name so descriptions align.
    std::size_t name_w = 0;
    for (const auto& [_, h] : rolls) name_w = std::max(name_w, h.name.size());
    for (const auto& [_, h] : rolls) {
      std::string pad_name = h.name;
      pad_name.append(name_w - pad_name.size(), ' ');
      std::println("{}  {}", pad_name,
                   h.description.empty() ? "(no description)" : h.description);
    }
    return 0;
  }

  // Detail mode.
  const std::string name{args[0]};
  const auto path = std::filesystem::path{rolesDir()} / (name + ".md");
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    std::println(stderr, "bus roles: no role file at {}", path.string());
    return 1;
  }
  const auto body = readBody(path);
  if (body.empty()) {
    std::println(stderr, "bus roles: {} is empty", path.string());
    return 1;
  }
  std::print("{}", body);
  return 0;
}

}  // namespace bus
