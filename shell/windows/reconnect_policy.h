// Reconnect timing, matching the Web viewer's schedule:
//   * one immediate retry;
//   * two retries one second later;
//   * from then on one attempt per minute, until the user stops it.
//
// `delay_for_attempt` takes the 1-based number of the attempt about to be
// made, so the sequence is 0, 1000, 1000, 60000, 60000, ...
#pragma once

#include <cstdint>

namespace molink::native::reconnect {

constexpr uint64_t kQuickDelayMs = 1000;
constexpr uint64_t kSlowDelayMs = 60000;

// The attempt that is about to run, 1-based.
uint64_t delay_for_attempt(int attempt);

/// True for the three quick attempts (the immediate one included).
bool is_quick_attempt(int attempt);

/// Whole seconds left until `deadline_ms`, rounded up; 0 when due.
int seconds_remaining(uint64_t deadline_ms, uint64_t now_ms);

}  // namespace molink::native::reconnect
