#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace molink {

/// Encodes one screen rectangle as a standalone AV1 keyframe (intra-only), so
/// every rectangle can be decoded on its own and drawn at its own x/y offset.
///
/// The rectangle is padded to a multiple of 8 pixels (and to the encoder's
/// 64-pixel minimum); the decoded picture has the padded size, and the caller
/// crops it back to the rectangle's real width and height.
class av1_encoder {
 public:
  virtual ~av1_encoder() = default;

  /// `bgra` is tightly packed BGRA with `stride` bytes per row (usually
  /// width * 4). `qp` is the AV1 constant quantizer: 0 is best quality, 63 the
  /// worst. `out` receives the AV1 OBUs. The encoder reconfigures itself when
  /// the size or qp changes.
  virtual bool encode_intra(const uint8_t* bgra, std::size_t stride, int width,
                            int height, int qp, std::vector<uint8_t>& out,
                            std::string& error) = 0;
};

/// Null (with `error` filled) when this build has no SVT-AV1 or the encoder
/// cannot be initialised. Callers must fall back to tile compression.
std::shared_ptr<av1_encoder> make_av1_encoder(std::string* error = nullptr);

/// Padded dimensions the encoder actually uses for a rectangle.
int av1_padded_extent(int extent);

}  // namespace molink
