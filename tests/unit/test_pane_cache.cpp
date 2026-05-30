#include "harness.h"
#include "pane.h"

#include <chrono>
#include <string>
#include <string_view>

using namespace bus;
using namespace std::chrono;

namespace {

// A cache wired to a controllable clock + a fetch-counting fetcher, so
// the TTL policy is testable without zellij or real time.
auto makeCache(int& fetches, steady_clock::time_point& clock,
               milliseconds ttl) -> PaneStateCache {
  return PaneStateCache{
      [&fetches](std::string_view n) {
        ++fetches;
        PaneState s;
        s.ok = true;
        s.mode = std::string{n};  // echo the name so hits are checkable
        return s;
      },
      [&clock] { return clock; },
      ttl};
}

}  // namespace

TEST(panecache_serves_hit_within_ttl) {
  int fetches = 0;
  steady_clock::time_point clock{};
  auto cache = makeCache(fetches, clock, milliseconds{300});

  auto a = cache.get("alice");
  CHECK_EQ(fetches, 1);
  CHECK_EQ(a.mode, std::string{"alice"});

  clock += milliseconds{100};  // still within TTL
  auto b = cache.get("alice");
  CHECK_EQ(fetches, 1);  // served from cache — no second fork
  CHECK_EQ(b.mode, std::string{"alice"});
}

TEST(panecache_refetches_after_ttl) {
  int fetches = 0;
  steady_clock::time_point clock{};
  auto cache = makeCache(fetches, clock, milliseconds{300});

  cache.get("x");
  CHECK_EQ(fetches, 1);

  clock += milliseconds{300};  // exactly TTL — not strictly fresher, refetch
  cache.get("x");
  CHECK_EQ(fetches, 2);

  clock += milliseconds{299};  // within TTL of the last fetch — hit
  cache.get("x");
  CHECK_EQ(fetches, 2);
}

TEST(panecache_names_are_independent) {
  int fetches = 0;
  steady_clock::time_point clock{};
  auto cache = makeCache(fetches, clock, milliseconds{300});

  cache.get("a");  // fetch
  cache.get("b");  // fetch (different key)
  cache.get("a");  // hit
  CHECK_EQ(fetches, 2);
}

TEST(panecache_zero_ttl_always_refetches) {
  int fetches = 0;
  steady_clock::time_point clock{};
  auto cache = makeCache(fetches, clock, milliseconds{0});

  cache.get("a");
  cache.get("a");  // 0 TTL: (now - at)=0 is not < 0ms — always refetch
  CHECK_EQ(fetches, 2);
}
