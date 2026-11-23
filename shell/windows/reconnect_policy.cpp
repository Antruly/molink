#include "reconnect_policy.h"

namespace molink::native::reconnect {

uint64_t delay_for_attempt(int attempt) {
  if (attempt <= 1) return 0;
  if (attempt <= 3) return kQuickDelayMs;
  return kSlowDelayMs;
}

bool is_quick_attempt(int attempt) { return attempt >= 1 && attempt <= 3; }

int seconds_remaining(uint64_t deadline_ms, uint64_t now_ms) {
  if (now_ms >= deadline_ms) return 0;
  const uint64_t left = deadline_ms - now_ms;
  return static_cast<int>((left + 999) / 1000);
}

}  // namespace molink::native::reconnect
