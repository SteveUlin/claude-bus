#include "ready.h"

#include "json_min.h"

#include <fstream>
#include <iterator>
#include <string>

namespace bus {

auto readReady(std::string_view state_root, std::string_view agent)
    -> std::optional<Ready> {
  const std::string path =
      std::string{state_root} + "/ready/" + std::string{agent} + ".json";
  std::ifstream in{path};
  if (!in) return std::nullopt;
  const std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
  const auto v = json::parse(content);
  if (!v || !v->isObject()) return std::nullopt;
  Ready r;
  r.session_id = v->getOrString("session_id");
  r.boundary_seq = v->getOrInt("boundary_seq", 0);
  r.ts_ms = v->getOrInt("ts_ms", 0);
  return r;
}

auto readyFresh(const Ready& r, std::int64_t now_ms, std::int64_t ttl_ms,
                std::int64_t last_event_ms) -> bool {
  if (now_ms - r.ts_ms >= ttl_ms) return false;   // expired
  if (r.ts_ms < last_event_ms) return false;        // superseded by newer event
  return true;
}

}  // namespace bus
