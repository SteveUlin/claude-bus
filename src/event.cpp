#include "event.h"

#include <cstdio>
#include <ctime>

namespace bus {

auto parseIso8601Ms(std::string_view s) -> std::int64_t {
  int Y = 0, M = 0, D = 0, h = 0, m = 0, sec = 0, ms = 0;
  if (std::sscanf(s.data(), "%4d-%2d-%2dT%2d:%2d:%2d.%3d", &Y, &M, &D, &h, &m,
                  &sec, &ms) != 7) {
    return 0;
  }
  std::tm tm{};
  tm.tm_year = Y - 1900;
  tm.tm_mon = M - 1;
  tm.tm_mday = D;
  tm.tm_hour = h;
  tm.tm_min = m;
  tm.tm_sec = sec;
  const std::time_t t = ::timegm(&tm);
  return static_cast<std::int64_t>(t) * 1000 + ms;
}

auto parseEvent(std::string_view line) -> std::optional<Event> {
  auto v = json::parse(line);
  if (!v || !v->isObject()) return std::nullopt;

  Event e;
  e.ts = v->getOrString("ts");
  e.session = v->getOrString("session");
  e.agent = v->getOrString("agent");
  e.pane = v->getOrString("pane");
  e.event = v->getOrString("event");
  e.ts_ms = parseIso8601Ms(e.ts);

  // Per-turn fields live under payload. Read from there; fall back to the
  // root only for msg_id (D3 may stamp it either place).
  if (const auto* p = v->get("payload"); p != nullptr && p->isObject()) {
    e.tool_name = p->getOrString("tool_name");
    e.source = p->getOrString("source");
    e.notification_type = p->getOrString("notification_type");
    e.transcript_path = p->getOrString("transcript_path");
    e.reason = p->getOrString("reason");
    e.prompt = p->getOrString("prompt");
    e.last_assistant_message = p->getOrString("last_assistant_message");
    e.cwd = p->getOrString("cwd");
    e.message = p->getOrString("message");
    e.msg_id = p->getOrString("msg_id");
    e.payload = *p;
  }
  if (e.msg_id.empty()) e.msg_id = v->getOrString("msg_id");

  return e;
}

}  // namespace bus
