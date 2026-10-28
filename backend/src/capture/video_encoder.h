#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace molink {

/// One bitstream the video pipeline can produce.
///
/// The capture picks a rung from this ladder per frame: rectangles as AV1
/// patches over a tile base image, or a low-delay stream of whole screens in
/// AV1 or H.264. Tiles and JPEG are not encoders -- they are the fallback the
/// transport always has.
enum class video_codec : uint8_t {
  av1_patches,  ///< intra-only AV1 rectangles over a tile base image
  av1_stream,   ///< low-delay AV1 stream: keyframe, then P frames
  h264_stream,  ///< low-delay H.264 stream: keyframe, then P frames
};

inline const char* video_codec_name(video_codec codec) {
  switch (codec) {
    case video_codec::av1_patches:
      return "av1-patches";
    case video_codec::av1_stream:
      return "av1-stream";
    case video_codec::h264_stream:
      return "h264-stream";
  }
  return "unknown";
}

/// What one encoder backend can produce on this machine.
///
/// This is the list a settings page (or a log line) can show: which backend is
/// present, whether it is hardware, and which rungs of the ladder it covers.
struct video_encoder_backend {
  std::string id;    ///< "gpu-nvenc" | "cpu-svt"
  std::string name;  ///< human-readable, e.g. "GPU · NVIDIA NVENC"
  bool hardware = false;
  bool available = false;
  bool av1_patches = false;
  bool av1_stream = false;
  bool h264_stream = false;
  std::string detail;
};

/// The common shape of every encoder the capture can drive (Phase 3).
///
/// The two concrete interfaces below (`av1_encoder` for rectangles,
/// `av1_stream_encoder` for streams) predate this type; both now derive from
/// it, so a backend can be described and chosen without the capture knowing
/// which half it implements. Backends implement the half they have and leave
/// the other returning false.
class video_encoder {
 public:
  virtual ~video_encoder() = default;

  /// The bitstream this encoder produces.
  virtual video_codec codec() const = 0;

  /// True when the pixels never touch the CPU (NVENC today).
  virtual bool hardware() const { return false; }

  /// Encodes one rectangle as a standalone picture (the patch path).
  /// `qp` is the codec's constant quantiser, `out` receives the packet.
  virtual bool encode_intra(const uint8_t* bgra, std::size_t stride, int width,
                            int height, int qp, std::vector<uint8_t>& out,
                            std::string& error) {
    (void)bgra;
    (void)stride;
    (void)width;
    (void)height;
    (void)qp;
    (void)out;
    error = std::string(video_codec_name(codec())) +
            ": this backend has no intra encoder";
    return false;
  }

  /// Encodes one full-screen frame of a low-delay stream. `out` may stay
  /// empty while the encoder holds a picture; `out_keyframe` says whether the
  /// packet it did produce starts a decodable chain.
  virtual bool encode_stream(const uint8_t* bgra, std::size_t stride, int width,
                             int height, int qp, bool keyframe,
                             std::vector<uint8_t>& out, bool& out_keyframe,
                             std::string& error) {
    (void)bgra;
    (void)stride;
    (void)width;
    (void)height;
    (void)qp;
    (void)keyframe;
    (void)out;
    (void)out_keyframe;
    error = std::string(video_codec_name(codec())) +
            ": this backend has no stream encoder";
    return false;
  }
};

/// The rungs the capture may use for this viewer set, best first.
///
/// A stream beats patches (a moving picture costs a fraction of the bytes), and
/// AV1 beats H.264 when both are decodable because it is smaller. The caller
/// walks this list and takes the first codec the host can actually encode.
inline std::vector<video_codec> video_ladder(bool av1_stream_ok,
                                             bool h264_stream_ok,
                                             bool av1_patches_ok) {
  std::vector<video_codec> ladder;
  if (av1_stream_ok) ladder.push_back(video_codec::av1_stream);
  if (h264_stream_ok) ladder.push_back(video_codec::h264_stream);
  if (av1_patches_ok) ladder.push_back(video_codec::av1_patches);
  return ladder;
}

/// Encoder backends present in this build and on this machine.
std::vector<video_encoder_backend> video_encoder_backends();

}  // namespace molink
