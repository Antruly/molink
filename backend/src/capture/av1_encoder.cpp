#include "capture/vpx_stream_encoder.h"
#include "capture/av1_encoder.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <functional>
#include <thread>
#include <type_traits>
#include <utility>

#if defined(MOLINK_HAVE_SVT_AV1)
#include <EbSvtAv1Enc.h>
#endif

#ifndef MOLINK_HAVE_NVENC
#define MOLINK_HAVE_NVENC 0
#endif
#if MOLINK_HAVE_NVENC
#include "capture/av1_stream_fallback.h"
#include "capture/nvenc_av1_encoder.h"
#endif

namespace molink {


int av1_padded_extent(int extent) {
  // SVT-AV1 wants at least 64 pixels and works on 8-pixel blocks for 4:2:0.
  const int minimum = std::max(extent, 64);
  return (minimum + 7) & ~7;
}

int av1_threads_for(int configured) {
  if (configured > 0) return std::clamp(configured, 1, 64);
  const unsigned cores = std::thread::hardware_concurrency();
  if (cores == 0) return 4;
  return std::clamp(static_cast<int>(cores) / 2, 2, 8);
}

int av1_enc_mode_for(const std::string& speed, int threads) {
  // 13 is the fastest SVT-AV1 preset and 10 is the sharpest of the real-time
  // range. A rectangle inside a capture frame cannot afford the slower ones.
  if (speed == "fast") return 13;
  if (speed == "balanced") return 12;
  if (speed == "quality") return 10;
  // Stay conservative: the previous default was 12 on every machine. Auto
  // only sharpens on very large CPUs and drops to the fastest preset on small
  // ones, so a rectangle still fits inside the capture frame budget.
  const int resolved = av1_threads_for(threads);
  if (resolved >= 8) return 11;
  if (resolved >= 4) return 12;
  return 13;
}

#if defined(MOLINK_HAVE_SVT_AV1)

namespace {

uint8_t clamp_u8(int value) {
  return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

/// Full-range BT.709 luma/chroma, matching the sequence header below.
inline void rgb_to_yuv(uint8_t r, uint8_t g, uint8_t b, uint8_t& y, int& cb,
                       int& cr) {
  y = clamp_u8((54 * r + 183 * g + 18 * b + 128) >> 8);
  cb = ((-29 * r - 99 * g + 128 * b + 128) >> 8) + 128;
  cr = ((128 * r - 116 * g - 12 * b + 128) >> 8) + 128;
}

// The public API changed between SVT-AV1 1.x (Ubuntu) and 4.x (vcpkg,
// Homebrew): handle creation lost its app-data argument, the IO struct lost
// width/height and the colour fields (the config carries them now), and two
// configuration fields were renamed. These overloads select the shape the
// installed header provides, so the same source builds against both.
template <typename Handle, typename Config>
auto init_handle(Handle& handle, Config& config, int)
    -> decltype(svt_av1_enc_init_handle(&handle, &config)) {
  return svt_av1_enc_init_handle(&handle, &config);
}
template <typename Handle, typename Config>
auto init_handle(Handle& handle, Config& config, long)
    -> decltype(svt_av1_enc_init_handle(&handle, nullptr, &config)) {
  return svt_av1_enc_init_handle(&handle, nullptr, &config);
}

template <typename Io>
auto set_io_size(Io& io, int width, int height, int)
    -> decltype(io.width = 0, void()) {
  io.width = static_cast<uint32_t>(width);
  io.height = static_cast<uint32_t>(height);
}
template <typename Io>
void set_io_size(Io&, int, int, long) {}

template <typename Io>
auto set_io_format(Io& io, int)
    -> decltype(io.color_fmt = EB_YUV420, void()) {
  io.color_fmt = EB_YUV420;
  io.bit_depth = EB_EIGHT_BIT;
}
template <typename Io>
void set_io_format(Io&, long) {}

template <typename Config>
auto disable_aq(Config& config, int)
    -> decltype(config.enable_adaptive_quantization = 0, void()) {
  config.enable_adaptive_quantization = 0;
}
template <typename Config>
auto disable_aq(Config& config, long) -> decltype(config.aq_mode = 0, void()) {
  config.aq_mode = 0;
}

template <typename Config>
auto set_parallelism(Config& config, int processors, int)
    -> decltype(config.logical_processors = 0, void()) {
  config.logical_processors = static_cast<uint32_t>(processors);
}
template <typename Config>
auto set_parallelism(Config& config, int processors, long)
    -> decltype(config.level_of_parallelism = 0, void()) {
  config.level_of_parallelism = static_cast<uint32_t>(processors);
}

/// Low-delay prediction is value 1 in both the old (LOW_DELAY_B) and the new
/// (LOW_DELAY) enum, so no version-specific enumerator is needed.
template <typename Config>
void set_low_delay(Config& config) {
  config.pred_structure = static_cast<decltype(config.pred_structure)>(1);
}

/// The 1.x handle API takes an app-data pointer, the 4.x one does not. The
/// same probe decides whether the encoder understands ALL_INTRA and the
/// 2-5 hierarchical-level range; 1.x rejects both.
template <typename H, typename C>
auto probe_two_arg_init(int)
    -> decltype(svt_av1_enc_init_handle(static_cast<H*>(nullptr),
                                        static_cast<C*>(nullptr)),
                std::true_type{});
template <typename H, typename C>
auto probe_two_arg_init(long) -> std::false_type;
constexpr bool kModernApi =
    decltype(probe_two_arg_init<EbComponentType*, EbSvtAv1EncConfiguration>(
        0))::value;

class svt_av1_encoder final : public av1_encoder {
 public:
  explicit svt_av1_encoder(av1_encoder_config config)
      : config_(std::move(config)) {}

  ~svt_av1_encoder() override { shutdown(); }

  bool encode_intra(const uint8_t* bgra, std::size_t stride, int width,
                    int height, int qp, std::vector<uint8_t>& out,
                    std::string& error) override {
    out.clear();
    if (bgra == nullptr || width <= 0 || height <= 0) {
      error = "av1: invalid rectangle";
      return false;
    }
    if (stride < static_cast<std::size_t>(width) * 4) {
      error = "av1: stride is shorter than a row";
      return false;
    }
    qp = std::clamp(qp, 1, 63);
    const int padded_w = av1_padded_extent(width);
    const int padded_h = av1_padded_extent(height);
    if (width_ != padded_w || height_ != padded_h || qp_ != qp) {
      shutdown();
      if (!init(padded_w, padded_h, qp, error)) return false;
    }
    if (!convert(bgra, stride, width, height, padded_w, padded_h, error)) {
      return false;
    }

    EbSvtIOFormat io{};
    io.luma = y_plane_.data();
    io.cb = u_plane_.data();
    io.cr = v_plane_.data();
    io.y_stride = static_cast<uint32_t>(padded_w);
    io.cb_stride = static_cast<uint32_t>(padded_w / 2);
    io.cr_stride = static_cast<uint32_t>(padded_w / 2);
    set_io_size(io, padded_w, padded_h, 0);
    set_io_format(io, 0);

    EbBufferHeaderType header{};
    header.size = sizeof(EbBufferHeaderType);
    header.p_buffer = reinterpret_cast<uint8_t*>(&io);
    header.n_filled_len =
        static_cast<uint32_t>(padded_w * padded_h * 3 / 2);
    header.pic_type = EB_AV1_KEY_PICTURE;

    // SVT-AV1's low-delay pipeline holds one picture before emitting output;
    // with a single picture it never flushes, even with pic_send_done. Send
    // the picture twice and use the first packet, then keep the duplicate in
    // the pipeline and discard its packet on the next call. The two pictures
    // are identical, so either packet would describe the same rectangle.
    if (!send(header, error)) return false;
    if (stale_pending_) {
      EbBufferHeaderType* stale = nullptr;
      if (!receive(stale, error)) return false;
      svt_av1_enc_release_out_buffer(&stale);
      stale_pending_ = false;
    }
    if (!send(header, error)) return false;
    EbBufferHeaderType* packet = nullptr;
    if (!receive(packet, error)) return false;
    if (packet->p_buffer != nullptr && packet->n_filled_len > 0) {
      out.assign(packet->p_buffer, packet->p_buffer + packet->n_filled_len);
    }
    svt_av1_enc_release_out_buffer(&packet);
    stale_pending_ = true;
    if (out.empty()) {
      error = "svt-av1 produced an empty packet";
      return false;
    }
    return true;
  }

 private:
  bool send(EbBufferHeaderType& header, std::string& error) {
    const EbErrorType sent = svt_av1_enc_send_picture(handle_, &header);
    if (sent != EB_ErrorNone) {
      error = "svt-av1 send_picture failed (" + std::to_string(sent) + ")";
      return false;
    }
    return true;
  }

  bool receive(EbBufferHeaderType*& packet, std::string& error) {
    packet = nullptr;
    for (int attempt = 0; attempt < 200 && packet == nullptr; ++attempt) {
      const EbErrorType got = svt_av1_enc_get_packet(handle_, &packet, 1);
      if (got == EB_NoErrorEmptyQueue) continue;
      if (got != EB_ErrorNone) {
        error = "svt-av1 get_packet failed (" + std::to_string(got) + ")";
        return false;
      }
    }
    if (packet == nullptr) {
      error = "svt-av1 produced no packet";
      return false;
    }
    return true;
  }

 private:
  bool init(int width, int height, int qp, std::string& error) {
    EbSvtAv1EncConfiguration config{};
    const EbErrorType handle_rc = init_handle(handle_, config, 0);
    if (handle_rc != EB_ErrorNone) {
      error = "svt-av1 init_handle failed (" + std::to_string(handle_rc) + ")";
      handle_ = nullptr;
      return false;
    }
    config.source_width = static_cast<uint32_t>(width);
    config.source_height = static_cast<uint32_t>(height);
    config.frame_rate_numerator = 30;
    config.frame_rate_denominator = 1;
    // The speed preset follows the user's choice; auto scales with the CPU.
    config.enc_mode = av1_enc_mode_for(config_.speed, config_.threads);
    config.intra_period_length = 1;
    if constexpr (kModernApi) {
      // 4.x: every picture is intra and hierarchical levels below 2 are
      // rejected. ALL_INTRA is value 0 in the new enum.
      config.pred_structure = static_cast<decltype(config.pred_structure)>(0);
      config.hierarchical_levels = 2;
    } else {
      set_low_delay(config);
      config.intra_refresh_type = SVT_AV1_KF_REFRESH;
      config.hierarchical_levels = 0;
    }
    config.rate_control_mode = SVT_AV1_RC_MODE_CQP_OR_CRF;
    config.qp = static_cast<uint32_t>(qp);
    disable_aq(config, 0);
    config.look_ahead_distance = 0;
    config.encoder_color_format = EB_YUV420;
    config.encoder_bit_depth = 8;
    config.color_primaries = EB_CICP_CP_BT_709;
    config.transfer_characteristics = EB_CICP_TC_BT_709;
    config.matrix_coefficients = EB_CICP_MC_BT_709;
    config.color_range = EB_CR_FULL_RANGE;
    config.fast_decode = 1;
    set_parallelism(config, av1_threads_for(config_.threads), 0);
    config.tile_columns = 0;
    config.tile_rows = 0;

    const EbErrorType param_rc = svt_av1_enc_set_parameter(handle_, &config);
    if (param_rc != EB_ErrorNone) {
      error = "svt-av1 set_parameter failed (" + std::to_string(param_rc) + ")";
      svt_av1_enc_deinit_handle(handle_);
      handle_ = nullptr;
      return false;
    }
    const EbErrorType init_rc = svt_av1_enc_init(handle_);
    if (init_rc != EB_ErrorNone) {
      error = "svt-av1 init failed (" + std::to_string(init_rc) + ")";
      svt_av1_enc_deinit_handle(handle_);
      handle_ = nullptr;
      return false;
    }
    width_ = width;
    height_ = height;
    qp_ = qp;
    stale_pending_ = false;
    y_plane_.assign(static_cast<std::size_t>(width) * height, 0);
    u_plane_.assign(static_cast<std::size_t>(width / 2) * (height / 2), 128);
    v_plane_.assign(static_cast<std::size_t>(width / 2) * (height / 2), 128);
    return true;
  }

  void shutdown() {
    y_plane_.clear();
    u_plane_.clear();
    v_plane_.clear();
    stale_pending_ = false;
    width_ = 0;
    height_ = 0;
    qp_ = 0;
    if (handle_ == nullptr) return;
    svt_av1_enc_deinit(handle_);
    svt_av1_enc_deinit_handle(handle_);
    handle_ = nullptr;
  }

  bool convert(const uint8_t* bgra, std::size_t stride, int width, int height,
               int padded_w, int padded_h, std::string& error) {
    if (width > padded_w || height > padded_h) {
      error = "av1: padded rectangle is smaller than the source";
      return false;
    }
    const std::size_t luma_size =
        static_cast<std::size_t>(padded_w) * padded_h;
    if (y_plane_.size() != luma_size) {
      y_plane_.assign(luma_size, 0);
    }
    const std::size_t chroma_size =
        static_cast<std::size_t>(padded_w / 2) * (padded_h / 2);
    if (u_plane_.size() != chroma_size) u_plane_.assign(chroma_size, 128);
    if (v_plane_.size() != chroma_size) v_plane_.assign(chroma_size, 128);

    // The padding repeats the last real row/column, which keeps the encoder
    // from spending bits on an artificial edge.
    for (int y = 0; y < padded_h; ++y) {
      const int src_y = std::min(y, height - 1);
      const uint8_t* row = bgra + static_cast<std::size_t>(src_y) * stride;
      uint8_t* luma = y_plane_.data() + static_cast<std::size_t>(y) * padded_w;
      for (int x = 0; x < padded_w; ++x) {
        const int src_x = std::min(x, width - 1);
        const uint8_t* px = row + static_cast<std::size_t>(src_x) * 4;
        uint8_t luma_value = 0;
        int cb = 0;
        int cr = 0;
        rgb_to_yuv(px[2], px[1], px[0], luma_value, cb, cr);
        luma[x] = luma_value;
      }
    }
    for (int y = 0; y < padded_h / 2; ++y) {
      uint8_t* u_row = u_plane_.data() +
                       static_cast<std::size_t>(y) * (padded_w / 2);
      uint8_t* v_row = v_plane_.data() +
                       static_cast<std::size_t>(y) * (padded_w / 2);
      for (int x = 0; x < padded_w / 2; ++x) {
        int cb_sum = 0;
        int cr_sum = 0;
        for (int dy = 0; dy < 2; ++dy) {
          const int src_y = std::min(y * 2 + dy, height - 1);
          const uint8_t* row =
              bgra + static_cast<std::size_t>(src_y) * stride;
          for (int dx = 0; dx < 2; ++dx) {
            const int src_x = std::min(x * 2 + dx, width - 1);
            const uint8_t* px = row + static_cast<std::size_t>(src_x) * 4;
            uint8_t luma_value = 0;
            int cb = 0;
            int cr = 0;
            rgb_to_yuv(px[2], px[1], px[0], luma_value, cb, cr);
            cb_sum += cb;
            cr_sum += cr;
          }
        }
        u_row[x] = clamp_u8(cb_sum / 4);
        v_row[x] = clamp_u8(cr_sum / 4);
      }
    }
    return true;
  }

  av1_encoder_config config_;
  EbComponentType* handle_ = nullptr;
  /// The duplicate picture left in SVT's one-frame pipeline by the last call.
  bool stale_pending_ = false;
  int width_ = 0;
  int height_ = 0;
  int qp_ = 0;
  std::vector<uint8_t> y_plane_;
  std::vector<uint8_t> u_plane_;
  std::vector<uint8_t> v_plane_;
};


/// Low-delay AV1 stream: one full-screen picture per call, P frames after the
/// first. The pipeline holds one picture, so a call returns the previous
/// frame's packet (or nothing on the very first call).
class svt_stream_encoder final : public av1_stream_encoder {
 public:
  explicit svt_stream_encoder(av1_encoder_config config)
      : config_(std::move(config)) {}
  ~svt_stream_encoder() override { shutdown(); }

  bool encode_stream(const uint8_t* bgra, std::size_t stride, int width,
                     int height, int qp, bool keyframe,
                     std::vector<uint8_t>& out, bool& out_keyframe,
                     std::string& error) override {
    out.clear();
    out_keyframe = false;
    if (bgra == nullptr || width <= 0 || height <= 0) {
      error = "av1 stream: invalid frame";
      return false;
    }
    if (stride < static_cast<std::size_t>(width) * 4) {
      error = "av1 stream: stride is shorter than a row";
      return false;
    }
    qp = std::clamp(qp, 1, 63);
    const int padded_w = av1_padded_extent(width);
    const int padded_h = av1_padded_extent(height);
    if (handle_ == nullptr || width_ != padded_w || height_ != padded_h ||
        qp_ != qp) {
      shutdown();
      if (!init(padded_w, padded_h, qp, error)) return false;
      keyframe_pending_ = true;  // a fresh encoder starts with a keyframe
    }
    if (keyframe && !keyframe_pending_) {
      // SVT 1.x ignores a KEY pic_type in low-delay mode; the only reliable
      // way to start a new reference chain is a fresh encoder, whose first
      // picture is always a keyframe. It costs one re-initialisation, which
      // happens only when a viewer joins or a decoder lost the stream.
      shutdown();
      if (!init(padded_w, padded_h, qp, error)) return false;
      keyframe_pending_ = true;
    }
    if (!convert(bgra, stride, width, height, padded_w, padded_h)) {
      error = "av1 stream: could not convert the frame";
      return false;
    }

    EbSvtIOFormat io{};
    io.luma = y_plane_.data();
    io.cb = u_plane_.data();
    io.cr = v_plane_.data();
    io.y_stride = static_cast<uint32_t>(padded_w);
    io.cb_stride = static_cast<uint32_t>(padded_w / 2);
    io.cr_stride = static_cast<uint32_t>(padded_w / 2);
    set_io_size(io, padded_w, padded_h, 0);
    set_io_format(io, 0);

    EbBufferHeaderType header{};
    header.size = sizeof(EbBufferHeaderType);
    header.p_buffer = reinterpret_cast<uint8_t*>(&io);
    header.n_filled_len = static_cast<uint32_t>(padded_w * padded_h * 3 / 2);
    const bool key = keyframe || keyframe_pending_;
    header.pic_type = key ? EB_AV1_KEY_PICTURE : EB_AV1_INTER_PICTURE;
    if (key) keyframe_pending_ = false;
    // The pipeline delays output by more than one picture, and the packet's
    // own pic_type does not tell the truth in this configuration, so the
    // keyframe flag travels with the picture through a small queue.
    pending_keyframes_.push_back(key);

    const EbErrorType sent = svt_av1_enc_send_picture(handle_, &header);
    if (sent != EB_ErrorNone) {
      error = "av1 stream: send_picture failed (" + std::to_string(sent) + ")";
      return false;
    }
    // Whatever the pipeline can give back now; the newest picture may still be
    // held, which simply means no packet for this frame.
    EbBufferHeaderType* packet = nullptr;
    for (int i = 0; i < 200; ++i) {
      const EbErrorType got = svt_av1_enc_get_packet(handle_, &packet, 0);
      if (got == EB_NoErrorEmptyQueue) break;
      if (got != EB_ErrorNone) {
        error = "av1 stream: get_packet failed (" + std::to_string(got) + ")";
        return false;
      }
      if (packet != nullptr) break;
    }
    if (packet != nullptr) {
      if (packet->p_buffer != nullptr && packet->n_filled_len > 0) {
        out.assign(packet->p_buffer, packet->p_buffer + packet->n_filled_len);
        if (!pending_keyframes_.empty()) {
          out_keyframe = pending_keyframes_.front();
          pending_keyframes_.pop_front();
        }
      }
      svt_av1_enc_release_out_buffer(&packet);
    }
    return true;
  }

 private:
  bool init(int width, int height, int qp, std::string& error) {
    EbSvtAv1EncConfiguration config{};
    const EbErrorType handle_rc = init_handle(handle_, config, 0);
    if (handle_rc != EB_ErrorNone) {
      error = "av1 stream: init_handle failed (" +
              std::to_string(handle_rc) + ")";
      handle_ = nullptr;
      return false;
    }
    config.source_width = static_cast<uint32_t>(width);
    config.source_height = static_cast<uint32_t>(height);
    config.frame_rate_numerator = 30;
    config.frame_rate_denominator = 1;
    config.enc_mode = av1_enc_mode_for(config_.speed, config_.threads);
    // Low-delay prediction: every picture references the one before it, and
    // a keyframe every few seconds bounds the recovery time after a loss.
    config.pred_structure = static_cast<decltype(config.pred_structure)>(1);
    config.hierarchical_levels = kModernApi ? 2 : 0;
    config.intra_period_length = 120;
    if constexpr (!kModernApi) {
      config.intra_refresh_type = SVT_AV1_KF_REFRESH;
    }
    config.rate_control_mode = SVT_AV1_RC_MODE_CQP_OR_CRF;
    config.qp = static_cast<uint32_t>(qp);
    disable_aq(config, 0);
    config.look_ahead_distance = 0;
    config.encoder_color_format = EB_YUV420;
    config.encoder_bit_depth = 8;
    config.color_primaries = EB_CICP_CP_BT_709;
    config.transfer_characteristics = EB_CICP_TC_BT_709;
    config.matrix_coefficients = EB_CICP_MC_BT_709;
    config.color_range = EB_CR_FULL_RANGE;
    set_parallelism(config, av1_threads_for(config_.threads), 0);
    config.tile_columns = 0;
    config.tile_rows = 0;

    const EbErrorType param_rc = svt_av1_enc_set_parameter(handle_, &config);
    if (param_rc != EB_ErrorNone) {
      error = "av1 stream: set_parameter failed (" +
              std::to_string(param_rc) + ")";
      svt_av1_enc_deinit_handle(handle_);
      handle_ = nullptr;
      return false;
    }
    const EbErrorType init_rc = svt_av1_enc_init(handle_);
    if (init_rc != EB_ErrorNone) {
      error = "av1 stream: init failed (" + std::to_string(init_rc) + ")";
      svt_av1_enc_deinit_handle(handle_);
      handle_ = nullptr;
      return false;
    }
    width_ = width;
    height_ = height;
    qp_ = qp;
    y_plane_.assign(static_cast<std::size_t>(width) * height, 0);
    u_plane_.assign(static_cast<std::size_t>(width / 2) * (height / 2), 128);
    v_plane_.assign(static_cast<std::size_t>(width / 2) * (height / 2), 128);
    return true;
  }

  void shutdown() {
    y_plane_.clear();
    u_plane_.clear();
    v_plane_.clear();
    keyframe_pending_ = true;
    pending_keyframes_.clear();
    width_ = 0;
    height_ = 0;
    qp_ = 0;
    if (handle_ == nullptr) return;
    svt_av1_enc_deinit(handle_);
    svt_av1_enc_deinit_handle(handle_);
    handle_ = nullptr;
  }

  bool convert(const uint8_t* bgra, std::size_t stride, int width, int height,
               int padded_w, int padded_h) {
    for (int y = 0; y < padded_h; ++y) {
      const int src_y = std::min(y, height - 1);
      const uint8_t* row = bgra + static_cast<std::size_t>(src_y) * stride;
      uint8_t* luma = y_plane_.data() + static_cast<std::size_t>(y) * padded_w;
      for (int x = 0; x < padded_w; ++x) {
        const int src_x = std::min(x, width - 1);
        const uint8_t* px = row + static_cast<std::size_t>(src_x) * 4;
        uint8_t ignored_u = 0;
        uint8_t ignored_v = 0;
        int cb = 0;
        int cr = 0;
        rgb_to_yuv(px[2], px[1], px[0], luma[x], cb, cr);
        (void)ignored_u;
        (void)ignored_v;
      }
    }
    for (int y = 0; y < padded_h / 2; ++y) {
      uint8_t* u_row = u_plane_.data() +
                       static_cast<std::size_t>(y) * (padded_w / 2);
      uint8_t* v_row = v_plane_.data() +
                       static_cast<std::size_t>(y) * (padded_w / 2);
      for (int x = 0; x < padded_w / 2; ++x) {
        int cb_sum = 0;
        int cr_sum = 0;
        for (int dy = 0; dy < 2; ++dy) {
          const int src_y = std::min(y * 2 + dy, height - 1);
          const uint8_t* row =
              bgra + static_cast<std::size_t>(src_y) * stride;
          for (int dx = 0; dx < 2; ++dx) {
            const int src_x = std::min(x * 2 + dx, width - 1);
            const uint8_t* px = row + static_cast<std::size_t>(src_x) * 4;
            uint8_t luma_value = 0;
            int cb = 0;
            int cr = 0;
            rgb_to_yuv(px[2], px[1], px[0], luma_value, cb, cr);
            cb_sum += cb;
            cr_sum += cr;
          }
        }
        u_row[x] = clamp_u8(cb_sum / 4);
        v_row[x] = clamp_u8(cr_sum / 4);
      }
    }
    return true;
  }

  av1_encoder_config config_;
  EbComponentType* handle_ = nullptr;
  bool keyframe_pending_ = true;
  /// One entry per picture sent, consumed per packet returned: the pipeline
  /// delays output, so the packet that arrives now was requested earlier.
  std::deque<bool> pending_keyframes_;
  int width_ = 0;
  int height_ = 0;
  int qp_ = 0;
  std::vector<uint8_t> y_plane_;
  std::vector<uint8_t> u_plane_;
  std::vector<uint8_t> v_plane_;
};

}  // namespace

std::vector<av1_encoder_device> av1_encoder_devices() {
  std::vector<av1_encoder_device> devices;
#if MOLINK_HAVE_NVENC
  for (auto& device : nvenc_av1_devices()) {
    devices.push_back(std::move(device));
  }
#endif
  devices.push_back({"cpu-svt", "CPU · SVT-AV1", false, true, ""});
  return devices;
}

bool av1_encode_available() { return true; }

std::vector<video_encoder_backend> video_encoder_backends() {
  std::vector<video_encoder_backend> backends;
#if MOLINK_HAVE_NVENC
  {
    video_encoder_backend gpu;
    gpu.id = "gpu-nvenc";
    gpu.name = "GPU · NVIDIA NVENC";
    gpu.hardware = true;
    gpu.av1_patches = nvenc_av1_available();
    gpu.av1_stream = gpu.av1_patches;
    gpu.h264_stream = nvenc_h264_available();
    gpu.available = gpu.av1_patches || gpu.h264_stream;
    gpu.detail = gpu.available ? "NVENC" : "no AV1/H.264 encoder was detected";
    // Only present backends are listed: callers treat the list as "what I can
    // encode with right now", not as a catalogue.
    if (gpu.available) backends.push_back(std::move(gpu));
  }
#endif
#if MOLINK_HAVE_VPX
  // libvpx covers VP9/VP8 when no hardware encoder is present.
  if (vpx_stream_available()) {
    video_encoder_backend vpx;
    vpx.id = "cpu-vpx";
    vpx.name = "CPU · libvpx";
    vpx.hardware = false;
    vpx.vp9_stream = true;
    vpx.vp8_stream = true;
    vpx.available = true;
    backends.push_back(std::move(vpx));
  }
#endif
  {
    video_encoder_backend cpu;
    cpu.id = "cpu-svt";
    cpu.name = "CPU · SVT-AV1";
    cpu.av1_patches = true;
    cpu.av1_stream = true;
    cpu.available = true;
    backends.push_back(std::move(cpu));
  }
  return backends;
}

namespace {

/// Runs the GPU encoder first and keeps CPU SVT-AV1 behind it. NVENC can
/// initialise and then fail while encoding a patch rectangle (driver session
/// limits, unusual rectangle sizes); a rectangle too small for NVENC goes
/// straight to the CPU backend, and the GPU is dropped only after repeated
/// failures so one bad strip cannot cost the session its hardware encoder.
class fallback_av1_encoder final : public av1_encoder {
 public:
  fallback_av1_encoder(std::shared_ptr<av1_encoder> first,
                       std::shared_ptr<av1_encoder> second)
      : first_(std::move(first)), second_(std::move(second)) {}

  bool encode_intra(const uint8_t* bgra, std::size_t stride, int width,
                    int height, int qp, std::vector<uint8_t>& out,
                    std::string& error) override {
    // NVENC refuses small rectangles; send those straight to the CPU backend
    // instead of letting one bad strip disable the GPU for the whole session.
    const bool gpu_sized = width >= 128 && height >= 128;
    if (first_ && gpu_sized) {
      std::string first_error;
      if (first_->encode_intra(bgra, stride, width, height, qp, out,
                               first_error)) {
        failures_ = 0;
        return true;
      }
      if (!reported_) {
        reported_ = true;
        std::fprintf(stderr,
                     "av1: nvenc failed for %dx%d qp=%d: %s\n", width, height,
                     qp, first_error.c_str());
      }
      if (++failures_ >= 8) first_.reset();
      error = first_error;
    }
    if (second_) {
      return second_->encode_intra(bgra, stride, width, height, qp, out, error);
    }
    return false;
  }

 private:
  std::shared_ptr<av1_encoder> first_;
  std::shared_ptr<av1_encoder> second_;
  int failures_ = 0;
  bool reported_ = false;
};

}  // namespace

std::shared_ptr<av1_encoder> make_av1_encoder(
    const av1_encoder_config& config, std::string* error) {
  if (error != nullptr) error->clear();
#if MOLINK_HAVE_NVENC
  if (config.device == "auto") {
    std::string gpu_error;
    if (auto gpu = make_nvenc_av1_encoder(config, &gpu_error)) {
      // One line so a host log answers "is this session on the GPU or the
      // CPU?" — the difference is an order of magnitude in frame time.
      std::fprintf(stderr, "av1: auto -> gpu-nvenc\n");
      return std::make_shared<fallback_av1_encoder>(
          std::move(gpu), std::make_shared<svt_av1_encoder>(config));
    }
  }
  if (config.device == "gpu-nvenc") {
    std::string gpu_error;
    if (auto gpu = make_nvenc_av1_encoder(config, &gpu_error)) {
      return gpu;
    }
    if (error != nullptr) *error = gpu_error;
    return nullptr;
  }
#endif
  if (config.device != "auto" && config.device != "cpu-svt") {
    if (error != nullptr) {
      *error = "av1: unsupported encoder device '" + config.device + "'";
    }
    return nullptr;
  }
  if (config.device == "auto") std::fprintf(stderr, "av1: auto -> cpu-svt\n");
  return std::make_shared<svt_av1_encoder>(config);
}

std::shared_ptr<av1_stream_encoder> make_stream_encoder(
    const av1_encoder_config& config, video_codec codec, std::string* error) {
  if (error != nullptr) error->clear();
  // libvpx is the only backend for VP8/VP9; NVENC has no encoder for them.
  if (codec == video_codec::vp9_stream || codec == video_codec::vp8_stream) {
    return make_vpx_stream_encoder(config, codec, error);
  }
#if MOLINK_HAVE_NVENC
  const bool wants_gpu =
      config.device == "auto" || config.device == "gpu-nvenc";
  if (wants_gpu) {
    std::string gpu_error;
    if (auto gpu = make_nvenc_stream_encoder(config, codec, &gpu_error)) {
      // One line so a host log answers "is the stream on the GPU?" -- the
      // difference is hundreds of milliseconds per frame against a few.
      std::fprintf(stderr, "stream: nvenc (%s)\n",
                   codec == video_codec::h264_stream ? "h264" : "av1");
      if (codec == video_codec::h264_stream) {
        // SVT-AV1 cannot continue an H.264 stream, so there is nothing to
        // hand over to: a failing GPU session ends the stream and the capture
        // falls back to tiles.
        return gpu;
      }
      return std::make_shared<fallback_av1_stream_encoder>(
          std::move(gpu), [config] {
            return std::make_shared<svt_stream_encoder>(config);
          });
    }
    if (config.device == "gpu-nvenc") {
      if (error != nullptr) *error = gpu_error;
      return nullptr;
    }
  }
#endif
  if (codec == video_codec::h264_stream) {
    // No CPU H.264 encoder in this build: the browser path needs the GPU.
    if (error != nullptr) {
      *error = "h264 stream: no hardware encoder is available on this host";
    }
    return nullptr;
  }
  if (config.device == "gpu-nvenc") {
    if (error != nullptr) *error = "nvenc: unavailable";
    return nullptr;
  }
  return std::make_shared<svt_stream_encoder>(config);
}

std::shared_ptr<av1_stream_encoder> make_av1_stream_encoder(
    const av1_encoder_config& config, std::string* error) {
  return make_stream_encoder(config, video_codec::av1_stream, error);
}

#else

std::vector<av1_encoder_device> av1_encoder_devices() {
#if MOLINK_HAVE_NVENC
  return nvenc_av1_devices();
#else
  return {};
#endif
}

bool av1_encode_available() {
#if MOLINK_HAVE_NVENC
  return nvenc_av1_available();
#else
  return false;
#endif
}

std::vector<video_encoder_backend> video_encoder_backends() {
  std::vector<video_encoder_backend> backends;
#if MOLINK_HAVE_NVENC
  video_encoder_backend gpu;
  gpu.id = "gpu-nvenc";
  gpu.name = "GPU · NVIDIA NVENC";
  gpu.hardware = true;
  gpu.av1_patches = nvenc_av1_available();
  gpu.av1_stream = gpu.av1_patches;
  gpu.h264_stream = nvenc_h264_available();
  gpu.available = gpu.av1_patches || gpu.h264_stream;
  if (gpu.available) backends.push_back(std::move(gpu));
#endif
#if MOLINK_HAVE_VPX
  // libvpx covers VP9/VP8 when no hardware encoder is present.
  if (vpx_stream_available()) {
    video_encoder_backend vpx;
    vpx.id = "cpu-vpx";
    vpx.name = "CPU · libvpx";
    vpx.hardware = false;
    vpx.vp9_stream = true;
    vpx.vp8_stream = true;
    vpx.available = true;
    backends.push_back(std::move(vpx));
  }
#endif
  return backends;
}

std::shared_ptr<av1_encoder> make_av1_encoder(
    const av1_encoder_config& config, std::string* error) {
  if (error != nullptr) error->clear();
#if MOLINK_HAVE_NVENC
  if (config.device == "auto" || config.device == "gpu-nvenc") {
    std::string gpu_error;
    if (auto gpu = make_nvenc_av1_encoder(config, &gpu_error)) {
      return gpu;
    }
    if (config.device == "gpu-nvenc") {
      if (error != nullptr) *error = gpu_error;
      return nullptr;
    }
  }
#endif
  if (error != nullptr) {
    *error = "this build has no SVT-AV1 encoder";
  }
  return nullptr;
}

std::shared_ptr<av1_stream_encoder> make_stream_encoder(
    const av1_encoder_config& config, video_codec codec, std::string* error) {
  if (error != nullptr) error->clear();
  // libvpx is the only backend for VP8/VP9; NVENC has no encoder for them.
  if (codec == video_codec::vp9_stream || codec == video_codec::vp8_stream) {
    return make_vpx_stream_encoder(config, codec, error);
  }
  // No SVT in this build, but the GPU can still carry the stream on its own.
#if MOLINK_HAVE_NVENC
  if (config.device == "auto" || config.device == "gpu-nvenc") {
    if (auto gpu = make_nvenc_stream_encoder(config, codec, error)) {
      std::fprintf(stderr, "stream: nvenc (%s)\n",
                   codec == video_codec::h264_stream ? "h264" : "av1");
      return gpu;
    }
    if (config.device == "gpu-nvenc") return nullptr;
  }
#endif
  if (error != nullptr) {
    *error = codec == video_codec::h264_stream
                 ? "this build has no H.264 stream encoder"
                 : "this build has no AV1 encoder for the stream";
  }
  return nullptr;
}

std::shared_ptr<av1_stream_encoder> make_av1_stream_encoder(
    const av1_encoder_config& config, std::string* error) {
  return make_stream_encoder(config, video_codec::av1_stream, error);
}

#endif  // MOLINK_HAVE_SVT_AV1

}  // namespace molink
