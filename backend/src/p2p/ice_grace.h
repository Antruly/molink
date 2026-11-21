#pragma once

#include <cstdint>

namespace molink {

/// Debounces libjuice's recoverable DISCONNECTED state before the session is
/// closed.
///
/// DISCONNECTED is not a terminal ICE state: a short consent failure, a Wi-Fi
/// roam or a NAT rebinding produces it while the selected pair is still there.
/// Closing the moment it appears turned a one-second blip into a permanent
/// relay downgrade. A DISCONNECTED that persists past `kGraceMs` is treated
/// like a failure, and FAILED is always immediate.
///
/// The machine fires at most once, so the caller can safely move its one-shot
/// callback out on the first `close_now`. Once fired, later observations are
/// ignored; the owner must also stop feeding it after destruction.
class ice_disconnect_grace final {
 public:
  static constexpr uint64_t kGraceMs = 5000;

  enum class result { none, close_now };

  /// Feed one observation. `failed` and `disconnected` describe the libjuice
  /// state; any other state cancels a pending grace period.
  result observe(bool failed, bool disconnected, uint64_t now_ms) {
    if (fired_) return result::none;
    if (failed) {
      fired_ = true;
      return result::close_now;
    }
    if (!disconnected) {
      disconnected_pending_ = false;
      disconnected_at_ms_ = 0;
      return result::none;
    }
    if (!disconnected_pending_) {
      disconnected_pending_ = true;
      disconnected_at_ms_ = now_ms;
      return result::none;
    }
    if (now_ms - disconnected_at_ms_ >= kGraceMs) {
      fired_ = true;
      return result::close_now;
    }
    return result::none;
  }

  bool fired() const { return fired_; }
  bool waiting() const { return !fired_ && disconnected_pending_; }

 private:
  bool disconnected_pending_ = false;
  uint64_t disconnected_at_ms_ = 0;
  bool fired_ = false;
};

}  // namespace molink
