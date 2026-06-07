#pragma once

#include "journal.h"

#include <string>
#include <string_view>

namespace bus {

// Per-consumer cursor persistence for a named topic log.
//
// CursorStore owns the $STATE/cursors/<name>/<consumer>.cursor files and
// the $STATE/cursors/<name>/<consumer>.lastid files that track each
// consumer's read head and last-acked record id.
//
// Construct with the same (state_root, name) pair that you'd previously pass
// to the two-arg Journal ctor. The journal path is derived internally via
// topicLogPath(state_root_, name_) for the cross-journal UUID tag check;
// byte ops (append/peek/dump/tail) go through a separately-constructed
// Journal built from that same path.
//
// All three methods preserve the exact semantics the old Journal cursor-store
// methods had:
//   - consumerCursor: read the persisted offset + stamp the journal's tag.
//   - ack: forward-only monotonic advance + optional lastid write.
//         Crashes (fatal) when a non-zero cursor tag doesn't match the
//         journal's tag (cross-journal ack is always a programmer error).
//   - lastAckedId: read the dedup floor id for a consumer.
class CursorStore {
 public:
  CursorStore(std::string state_root, std::string name);

  // Read the persisted cursor for `consumer` ("" = default). Returns the
  // start-of-log Cursor when no cursor file exists yet. The returned Cursor
  // carries the journal's binding tag (from the on-disk header UUID), or
  // tag 0 if the journal file doesn't exist yet.
  auto consumerCursor(std::string_view consumer = "") const -> Journal::Cursor;

  // Advance the consumer cursor to `target` — forward-only; a backward or
  // equal target is a no-op (at-least-once invariant). Stamps the last-acked
  // id when `id` is non-empty. Returns true iff it actually wrote.
  //
  // FATAL: target's tag is non-zero and != the journal's own tag
  //        (cross-journal cursor — always a programmer error).
  auto ack(std::string_view consumer, Journal::Cursor target,
           std::string_view id = "") -> bool;

  // Read the last-acked record id for `consumer` (the dedup floor).
  // Empty string when none has been stamped.
  auto lastAckedId(std::string_view consumer = "") const -> std::string;

 private:
  std::string state_root_;
  std::string name_;

  auto cursorFilePath(std::string_view consumer) const -> std::string;
  auto lastIdFilePath(std::string_view consumer) const -> std::string;
};

}  // namespace bus
