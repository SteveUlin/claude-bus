#include "token_watcher.h"

#include "agent_status.h"  // readAgents, nowMs
#include "json_min.h"
#include "pane.h"          // paneId
#include "tail_reader.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace bus {

TokenWatcher::TokenWatcher(std::string state_dir)
    : state_dir_{std::move(state_dir)} {}

// Token-scan watcher. Replaces the statusline script's data-write side
// effect. For each live agent, tail its
// transcript JSONL, compute context occupancy from the last assistant
// turn's usage, and write $STATE/status/<agent>.json — the CTX% source
// `bus monitor` reads. Only two fields are consumed downstream
// (used_percentage + context_window_size), so that's all we write.
//
// Numerator (context tokens) comes straight from the transcript and
// matches the statusline's context_window.total_input_tokens exactly.
// The denominator (window size) is NOT in the transcript anywhere, so
// it comes straight from CLAUDE_BUS_CTX_WINDOW (default 200k; the fleet
// layout sets the real window).
auto TokenWatcher::scan() -> void {
  const auto now = nowMs();
  if (now - token_scan_last_ms_ < 5'000) return;  // every 5 s
  token_scan_last_ms_ = now;

  std::int64_t configured_window = 200'000;
  if (const char* env = std::getenv("CLAUDE_BUS_CTX_WINDOW");
      env != nullptr && *env != '\0') {
    if (const auto v = std::atoll(env); v > 0) configured_window = v;
  }

  const std::string events_log = state_dir_ + "/events.jsonl";
  auto agents = readAgents(events_log, {});

  for (const auto& [name, info] : agents) {
    const auto& path = info.last.transcript_path;
    if (path.empty()) continue;
    // Live-pane gate — skip ghost markers from dead sessions.
    if (paneId(name).empty()) continue;

    auto& sc = token_scan_[name];
    // New session (post /clear or /compact gets a fresh session UUID,
    // hence a new transcript path) → re-read from the top.
    if (sc.path != path) {
      sc.path = path;
      sc.offset = 0;
      sc.last_tokens = -1;
    }

    // Incremental tail read via the shared TailReader: parse only
    // assistant lines appended since the last scan, keeping the most
    // recent turn's occupancy. poll() owns the offset advance + torn-tail
    // refusal that this loop used to hand-roll with tellg.
    bus::TailReader reader{path};
    reader.setOffset(sc.offset);
    for (auto& line : reader.poll()) {
      // Cheap substring pre-filter before the JSON parse.
      if (line.find("\"type\":\"assistant\"") == std::string::npos) continue;
      if (line.find("\"usage\"") == std::string::npos) continue;
      auto v = json::parse(line);
      if (!v || !v->isObject()) continue;
      const auto* msg = v->get("message");
      if (msg == nullptr || !msg->isObject()) continue;
      const auto* usage = msg->get("usage");
      if (usage == nullptr || !usage->isObject()) continue;
      // Context occupancy = non-output tokens.
      sc.last_tokens = usage->getOrInt("input_tokens", 0) +
                       usage->getOrInt("cache_creation_input_tokens", 0) +
                       usage->getOrInt("cache_read_input_tokens", 0);
      // Model rides the same assistant turn. Skip <synthetic> turns (they
      // carry no real model) so the column sticks to the last real model.
      if (const auto m = msg->getOrString("model");
          !m.empty() && m != "<synthetic>") {
        sc.last_model = m;
      }
    }
    sc.offset = reader.offset();

    if (sc.last_tokens < 0) continue;  // no assistant turn yet

    // Denominator is just the configured window (CLAUDE_BUS_CTX_WINDOW;
    // the fleet layout sets it to the real window). The old two-tier
    // ">200k ? 1M : 200K" escalation guess is gone — it mis-reported a
    // 1M-window agent sitting under 200k tokens, and the knob is the
    // honest source. pct is clamped to [0,100] so a misconfigured knob
    // can't produce a nonsense number.
    const auto window = configured_window;
    auto pct = (sc.last_tokens * 100 + window / 2) / window;  // rounded
    if (pct > 100) pct = 100;
    if (pct < 0) pct = 0;

    // Atomic write of the two fields the monitor reads. The
    // context_window block matches the old statusline projection's
    // shape so the existing scanIntAfter readers keep working.
    const std::string dir = state_dir_ + "/status";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string final_path = dir + "/" + name + ".json";
    const std::string tmp_path =
        final_path + ".tmp." + std::to_string(::getpid());
    {
      std::ofstream out{tmp_path, std::ios::trunc};
      if (!out) continue;
      // context_tokens = the RAW occupancy numerator (kvothe's producer
      // contract): un-rounded so P2 can compute a token-granular Δtokens/Δt
      // over its own window (used_percentage's integer %s are too coarse — at
      // a 1M window 1% = 10k tokens, so a slow agent reads as 0% delta).
      // model feeds kvothe's MODEL column. Both pair with the existing ts.
      out << std::format(
          "{{\"agent\":\"{}\",\"ts\":{},\"model\":\"{}\",\"context_tokens\":{},"
          "\"context_window\":"
          "{{\"used_percentage\":{},\"context_window_size\":{}}}}}\n",
          name, now, sc.last_model, sc.last_tokens, pct, window);
    }
    fs::rename(tmp_path, final_path, ec);
    if (ec) fs::remove(tmp_path, ec);
  }
}

auto TokenWatcher::pruneDead(const std::set<std::string>& live_agents) -> void {
  std::vector<std::string> to_erase;
  for (const auto& [k, _] : token_scan_) {
    if (!live_agents.contains(k)) to_erase.push_back(k);
  }
  for (const auto& k : to_erase) token_scan_.erase(k);
}

}  // namespace bus
