#pragma once

#include <cstdint>
#include <vector>

#include "protocol/frame_packet.h"

namespace molink {

/// Composes the tile stream a viewer core receives into one BGRA canvas and
/// publishes it to the native frame surface, so the shell can draw the remote
/// screen without the WebView in the path.
///
/// Only the self-contained tile codecs are handled for now (raw and deflated
/// BGRA/RGB24/RGB565/gray8); the low-delay streams (AV1/VP9/VP8/H.264/JPEG)
/// need a decoder in the core and stay on the page until that lands.
class surface_compositor {
 public:
  /// Integrates one parsed frame. Returns true when the canvas was published.
  bool ingest(const protocol::parsed_frame& frame);

  /// Drops the canvas (a resolution change or a new session).
  void reset();

  int width() const { return width_; }
  int height() const { return height_; }
  uint64_t published() const { return published_; }

 private:
  bool ensure(int width, int height);

  std::vector<uint8_t> canvas_;
  int width_ = 0;
  int height_ = 0;
  uint64_t published_ = 0;
  /// Scratch buffer for deflated tiles, reused across frames.
  std::vector<uint8_t> inflated_;
};

/// Feed one serialized packet to the compositor. Returns true when a picture
/// was published. A cheap no-op for control packets and while the native
/// surface is off.
bool surface_ingest_packet(surface_compositor& compositor,
                           const uint8_t* data, std::size_t len);

}  // namespace molink
