#include "sub.h"

#include <cstdio>
#include <print>
#include <string_view>

namespace bus {

auto subMsg(std::span<const char* const> args) -> int {
  if (args.empty()) {
    std::println(stderr, "usage: bus msg <cmd> [args...]");
    std::println(stderr, "");
    std::println(stderr, "commands:");
    std::println(stderr, "  send       name->pane TTY write");
    std::println(stderr, "  fetch      pop the next record from a topic");
    std::println(stderr, "  drain      off-TTY: pull inbox-AGENT as additionalContext JSON");
    std::println(stderr, "  mail       enqueue text to inbox-AGENT");
    std::println(stderr, "  slash      enqueue a slash command to commands-AGENT");
    std::println(stderr, "  broadcast  fan out one body into many inboxes");
    std::println(stderr, "  enqueue    publish a record to a topic");
    std::println(stderr, "  peek       read records from a topic without consuming");
    std::println(stderr, "  body       fetch a record body by msg_id");
    std::println(stderr, "  drop       advance cursor past msg_id without delivering (audited)");
    return 1;
  }

  const std::string_view cmd = args[0];
  const auto sub_args = args.subspan(1);

  if (cmd == "send") return subSend(sub_args);
  if (cmd == "fetch") return subFetch(sub_args);
  if (cmd == "drain") return subDrain(sub_args);
  if (cmd == "mail") return subMail(sub_args);
  if (cmd == "slash") return subSlash(sub_args);
  if (cmd == "broadcast") return subBroadcast(sub_args);
  if (cmd == "enqueue") return subEnqueue(sub_args);
  if (cmd == "peek") return subPeek(sub_args);
  if (cmd == "body") return subBody(sub_args);
  if (cmd == "drop") return subDrop(sub_args);

  std::println(stderr, "bus msg: unknown command \"{}\"", cmd);
  return 2;
}

}  // namespace bus
