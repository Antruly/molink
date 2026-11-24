#pragma once

namespace molink {

/// Remembers the rectangle last handed to the input injector.
///
/// The capture reports the monitor geometry, which changes when the host
/// switches resolution or rearranges displays. The injector must follow it or
/// every click lands at the old offset. Keeping the comparison pure makes the
/// "only update when it really moved" rule testable without an X server.
class display_rect_state {
 public:
  struct rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool operator==(const rect& other) const {
      return x == other.x && y == other.y && width == other.width &&
             height == other.height;
    }
  };

  /// Returns true when @p width x @p height at (@p x, @p y) differs from the
  /// rectangle already remembered. A non-positive size is ignored (the capture
  /// may not have a frame yet).
  bool update(int x, int y, int width, int height) {
    if (width <= 0 || height <= 0) return false;
    const rect next{x, y, width, height};
    if (has_rect_ && next == rect_) return false;
    rect_ = next;
    has_rect_ = true;
    return true;
  }

  bool has_rect() const { return has_rect_; }
  const rect& current() const { return rect_; }

 private:
  rect rect_{};
  bool has_rect_ = false;
};

}  // namespace molink
