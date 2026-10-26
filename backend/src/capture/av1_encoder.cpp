#include "capture/av1_encoder.h"

#include <algorithm>
#include <cstring>

namespace molink {

int av1_padded_extent(int extent) {
  // SVT-AV1 wants at least 64 pixels and works on 8-pixel blocks for 4:2:0.
  const int minimum = std::max(extent, 64);
  return (minimum + 7) & ~7;
}

#if defined(MOLINK_HAVE_SVT_AV1)

#include <EbSvtAv1Enc.h>

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

class svt_av1_encoder final : public av1_encoder {
 public:
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
    io.width = static_cast<uint32_t>(padded_w);
    io.height = static_cast<uint32_t>(padded_h);
    io.color_fmt = EB_YUV420;
    io.bit_depth = EB_EIGHT_BIT;

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
    const EbErrorType handle_rc =
        svt_av1_enc_init_handle(&handle_, nullptr, &config);
    if (handle_rc != EB_ErrorNone) {
      error = "svt-av1 init_handle failed (" + std::to_string(handle_rc) + ")";
      handle_ = nullptr;
      return false;
    }
    config.source_width = static_cast<uint32_t>(width);
    config.source_height = static_cast<uint32_t>(height);
    config.frame_rate_numerator = 30;
    config.frame_rate_denominator = 1;
    // 13 is the fastest preset; 10 keeps a little more quality while still
    // fitting a rectangle in a frame budget on a desktop CPU.
    config.enc_mode = 10;
    config.pred_structure = SVT_AV1_PRED_LOW_DELAY_B;
    config.intra_period_length = 1;
    config.intra_refresh_type = SVT_AV1_KF_REFRESH;
    config.hierarchical_levels = 0;
    config.rate_control_mode = SVT_AV1_RC_MODE_CQP_OR_CRF;
    config.qp = static_cast<uint32_t>(qp);
    config.enable_adaptive_quantization = 0;
    config.look_ahead_distance = 0;
    config.encoder_color_format = EB_YUV420;
    config.encoder_bit_depth = 8;
    config.color_primaries = EB_CICP_CP_BT_709;
    config.transfer_characteristics = EB_CICP_TC_BT_709;
    config.matrix_coefficients = EB_CICP_MC_BT_709;
    config.color_range = EB_CR_FULL_RANGE;
    config.fast_decode = 1;
    config.logical_processors = 4;
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

}  // namespace

std::shared_ptr<av1_encoder> make_av1_encoder(std::string* error) {
  if (error != nullptr) error->clear();
  return std::make_shared<svt_av1_encoder>();
}

#else

std::shared_ptr<av1_encoder> make_av1_encoder(std::string* error) {
  if (error != nullptr) {
    *error = "this build has no SVT-AV1 encoder";
  }
  return nullptr;
}

#endif  // MOLINK_HAVE_SVT_AV1

}  // namespace molink
