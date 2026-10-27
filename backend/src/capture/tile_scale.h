#pragma once

#include <cstdint>
#include <vector>

namespace molink {

/// Replaces every 2x2 block of a BGRA image with its average colour, keeping
/// the image at full size. The picture gets softer while it moves, but the
/// stream deflates much smaller and the viewer inflates less: the client needs
/// no changes, because the tile is still delivered at its declared size.
///
/// Used for large changes on a link that cannot carry full detail, the tile
/// path's counterpart to the AV1 half-resolution encode.
inline void downsample_box2_bgra(std::vector<uint8_t>& bgra, int width,
                                 int height) {
  if (width < 2 || height < 2) return;
  if (bgra.size() < static_cast<std::size_t>(width) * height * 4) return;
  const std::size_t stride = static_cast<std::size_t>(width) * 4;
  for (int y = 0; y + 1 < height; y += 2) {
    uint8_t* row0 = bgra.data() + static_cast<std::size_t>(y) * stride;
    uint8_t* row1 = row0 + stride;
    for (int x = 0; x + 1 < width; x += 2) {
      uint8_t* a = row0 + static_cast<std::size_t>(x) * 4;
      uint8_t* b = row1 + static_cast<std::size_t>(x) * 4;
      uint8_t average[4];
      for (int channel = 0; channel < 4; ++channel) {
        average[channel] = static_cast<uint8_t>(
            (a[channel] + a[channel + 4] + b[channel] + b[channel + 4] + 2) /
            4);
      }
      for (int channel = 0; channel < 4; ++channel) {
        a[channel] = average[channel];
        a[channel + 4] = average[channel];
        b[channel] = average[channel];
        b[channel + 4] = average[channel];
      }
    }
  }
}

}  // namespace molink
