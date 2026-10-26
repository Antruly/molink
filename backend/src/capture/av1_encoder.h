#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace molink {

/// One encoder backend this build can use. "auto" prefers "gpu-nvenc" (NVENC)
/// when the driver advertises AV1 encoding and falls back to "cpu-svt".
struct av1_encoder_device {
  std::string id;
  std::string name;
  bool hardware = false;
  bool available = false;
  std::string detail;
};

/// Per-encoder configuration. `speed` is "auto" | "fast" | "balanced" |
/// "quality"; `threads` 0 means "derive from the CPU".
struct av1_encoder_config {
  std::string device = "auto";
  std::string speed = "auto";
  int threads = 0;
};

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

/// Null (with `error` filled) when this build has no SVT-AV1, the requested
/// device is unknown, or the encoder cannot be initialised. Callers must fall
/// back to tile compression.
std::shared_ptr<av1_encoder> make_av1_encoder(const av1_encoder_config& config,
                                             std::string* error = nullptr);

/// Encoders available in this build, best first. Empty when AV1 encoding was
/// compiled out.
std::vector<av1_encoder_device> av1_encoder_devices();

/// True when this build can encode AV1 rectangles (SVT-AV1 is linked).
bool av1_encode_available();

/// Resolve the SVT-AV1 enc_mode for a speed name and thread count. Auto picks
/// a faster preset on small machines and a sharper one on large ones.
int av1_enc_mode_for(const std::string& speed, int threads);

/// Resolve the configured thread count: 0 follows the CPU (roughly half of
/// the logical processors, clamped to 2..8).
int av1_threads_for(int configured);

/// Padded dimensions the encoder actually uses for a rectangle.
int av1_padded_extent(int extent);

}  // namespace molink
