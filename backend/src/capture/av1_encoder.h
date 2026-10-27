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

/// Encodes a low-delay AV1 stream of full-screen frames: the first picture is
/// a keyframe and later ones are P frames that only carry what changed. This
/// is what makes a moving 1080p screen affordable, but the receiver has to
/// keep a matching decoder alive -- a frame cannot be decoded on its own.
class av1_stream_encoder {
 public:
  virtual ~av1_stream_encoder() = default;

  /// Encodes one full-screen frame. `out` is empty when the encoder has no
  /// packet ready yet (its low-delay pipeline holds one picture), which is
  /// normal for the first frame. `keyframe` forces this picture to be a
  /// keyframe, which is also what a receiver needs after a decode failure.
  virtual bool encode_stream(const uint8_t* bgra, std::size_t stride, int width,
                             int height, int qp, bool keyframe,
                             std::vector<uint8_t>& out, bool& out_keyframe,
                             std::string& error) = 0;
};

/// Null (with `error` filled) when this build has no encoder for the stream.
std::shared_ptr<av1_stream_encoder> make_av1_stream_encoder(
    const av1_encoder_config& config, std::string* error = nullptr);

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
