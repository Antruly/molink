/// Low-delay VP9/VP8 streams through libvpx.
///
/// The rung exists for hosts or viewers where the hardware codecs are not
/// available: VP9 compresses better than H.264 in software, and every browser
/// decodes VP8/VP9 through WebCodecs. The encoder runs one-pass realtime with
/// no look-ahead and no automatic keyframes -- the capture decides when a
/// keyframe is needed, exactly like the NVENC and SVT streams.

#include "capture/vpx_stream_encoder.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#if MOLINK_HAVE_VPX

#include <vpx/vp8cx.h>
#include <vpx/vpx_encoder.h>

namespace molink {

namespace {

/// BGRA to I420 with the same full-range BT.709 weights the AV1 paths use, so
/// the two codecs produce the same colours for the same screen.
void bgra_to_i420(const uint8_t* bgra, std::size_t stride, int width,
                  int height, int padded_w, int padded_h,
                  std::vector<uint8_t>& out) {
  const int chroma_w = (padded_w + 1) / 2;
  const int chroma_h = (padded_h + 1) / 2;
  out.assign(static_cast<std::size_t>(padded_w) * padded_h +
                 2 * static_cast<std::size_t>(chroma_w) * chroma_h,
             128);
  uint8_t* y_plane = out.data();
  uint8_t* u_plane = out.data() + static_cast<std::size_t>(padded_w) * padded_h;
  uint8_t* v_plane = u_plane + static_cast<std::size_t>(chroma_w) * chroma_h;

  for (int y = 0; y < padded_h; ++y) {
    const int src_y = (std::min)(y, height - 1);
    const uint8_t* row = bgra + static_cast<std::size_t>(src_y) * stride;
    uint8_t* y_row = y_plane + static_cast<std::size_t>(y) * padded_w;
    for (int x = 0; x < padded_w; ++x) {
      const int src_x = (std::min)(x, width - 1);
      const uint8_t* px = row + static_cast<std::size_t>(src_x) * 4;
      const int b = px[0];
      const int g = px[1];
      const int r = px[2];
      y_row[x] = static_cast<uint8_t>((77 * r + 150 * g + 29 * b) >> 8);
    }
  }
  // Chroma: box-average each 2x2 block so edges do not shimmer.
  for (int cy = 0; cy < chroma_h; ++cy) {
    for (int cx = 0; cx < chroma_w; ++cx) {
      int sum_r = 0;
      int sum_g = 0;
      int sum_b = 0;
      int count = 0;
      for (int dy = 0; dy < 2; ++dy) {
        const int y = (std::min)(cy * 2 + dy, height - 1);
        const uint8_t* row = bgra + static_cast<std::size_t>(y) * stride;
        for (int dx = 0; dx < 2; ++dx) {
          const int x = (std::min)(cx * 2 + dx, width - 1);
          const uint8_t* px = row + static_cast<std::size_t>(x) * 4;
          sum_b += px[0];
          sum_g += px[1];
          sum_r += px[2];
          ++count;
        }
      }
      const int r = sum_r / count;
      const int g = sum_g / count;
      const int b = sum_b / count;
      const int luma = (77 * r + 150 * g + 29 * b) >> 8;
      const int u = ((-13 * r - 25 * g + 38 * b) >> 8) + 128;
      const int v = ((38 * r - 32 * g - 6 * b) >> 8) + 128;
      (void)luma;
      u_plane[static_cast<std::size_t>(cy) * chroma_w + cx] =
          static_cast<uint8_t>(std::clamp(u, 0, 255));
      v_plane[static_cast<std::size_t>(cy) * chroma_w + cx] =
          static_cast<uint8_t>(std::clamp(v, 0, 255));
    }
  }
}

class vpx_stream_encoder final : public av1_stream_encoder {
 public:
  vpx_stream_encoder(av1_encoder_config config, video_codec codec)
      : config_(std::move(config)), codec_(codec) {}

  ~vpx_stream_encoder() override { destroy(); }

  video_codec codec() const override { return codec_; }

  bool encode_stream(const uint8_t* bgra, std::size_t stride, int width,
                     int height, int qp, bool keyframe,
                     std::vector<uint8_t>& out, bool& out_keyframe,
                     std::string& error) override {
    out.clear();
    out_keyframe = false;
    if (bgra == nullptr || width <= 0 || height <= 0 ||
        stride < static_cast<std::size_t>(width) * 4) {
      error = "vpx: invalid stream frame";
      return false;
    }
    const int padded_w = (std::max)(2, (width + 1) & ~1);
    const int padded_h = (std::max)(2, (height + 1) & ~1);
    const int vpx_qp = std::clamp(qp, 1, 63);
    if (!open_ || padded_w != width_ || padded_h != height_ ||
        vpx_qp != qp_) {
      if (!init(padded_w, padded_h, vpx_qp, error)) return false;
      // A fresh encoder has no reference picture.
      keyframe = true;
    }

    bgra_to_i420(bgra, stride, width, height, padded_w, padded_h, i420_);
    const int chroma_w = (padded_w + 1) / 2;
    const int chroma_h = (padded_h + 1) / 2;
    vpx_image_t image;
    if (vpx_img_wrap(&image, VPX_IMG_FMT_I420, padded_w, padded_h, 1,
                     i420_.data()) == nullptr) {
      error = "vpx: could not wrap the picture";
      return false;
    }
    image.planes[VPX_PLANE_U] = i420_.data() +
                                static_cast<std::size_t>(padded_w) * padded_h;
    image.planes[VPX_PLANE_V] =
        image.planes[VPX_PLANE_U] +
        static_cast<std::size_t>(chroma_w) * chroma_h;
    image.stride[VPX_PLANE_Y] = padded_w;
    image.stride[VPX_PLANE_U] = chroma_w;
    image.stride[VPX_PLANE_V] = chroma_w;

    const vpx_enc_frame_flags_t flags =
        keyframe ? VPX_EFLAG_FORCE_KF : 0;
    const vpx_codec_err_t rc = vpx_codec_encode(
        &ctx_, &image, pts_++, 1, flags, VPX_DL_REALTIME);
    if (rc != VPX_CODEC_OK) {
      error = std::string("vpx: encode failed: ") +
              (vpx_codec_error(&ctx_) != nullptr ? vpx_codec_error(&ctx_)
                                                 : "unknown");
      if (vpx_codec_error_detail(&ctx_) != nullptr) {
        error += std::string(" (") + vpx_codec_error_detail(&ctx_) + ")";
      }
      return false;
    }
    vpx_codec_iter_t iter = nullptr;
    bool any = false;
    for (const vpx_codec_cx_pkt_t* pkt = vpx_codec_get_cx_data(&ctx_, &iter);
         pkt != nullptr; pkt = vpx_codec_get_cx_data(&ctx_, &iter)) {
      if (pkt->kind != VPX_CODEC_CX_FRAME_PKT) continue;
      const auto* data = static_cast<const uint8_t*>(pkt->data.frame.buf);
      out.insert(out.end(), data, data + pkt->data.frame.sz);
      any = true;
      if ((pkt->data.frame.flags & VPX_FRAME_IS_KEY) != 0) {
        out_keyframe = true;
      }
    }
    if (!any) {
      // The encoder kept the picture; an empty packet is legal on this
      // interface and the capture answers with an empty frame.
      out.clear();
      return true;
    }
    if (!out_keyframe && keyframe) out_keyframe = true;
    return true;
  }

 private:
  bool init(int width, int height, int qp, std::string& error) {
    destroy();
    const vpx_codec_iface_t* iface = codec_ == video_codec::vp9_stream
                                         ? vpx_codec_vp9_cx()
                                         : vpx_codec_vp8_cx();
    vpx_codec_enc_cfg_t cfg;
    if (vpx_codec_enc_config_default(iface, &cfg, 0) != VPX_CODEC_OK) {
      error = "vpx: no default configuration";
      return false;
    }
    cfg.g_w = static_cast<unsigned int>(width);
    cfg.g_h = static_cast<unsigned int>(height);
    cfg.g_timebase.num = 1;
    cfg.g_timebase.den = 30;
    cfg.g_pass = VPX_RC_ONE_PASS;
    cfg.g_lag_in_frames = 0;
    cfg.rc_end_usage = VPX_CQ;
    // The constant-quality level is set through the codec control below; the
    // quantizer bounds stay wide because VP8 rejects a CQ level outside them
    // ("cq_level out of range").
    cfg.rc_min_quantizer = 0;
    cfg.rc_max_quantizer = 63;
    cfg.rc_target_bitrate = 4000;
    cfg.kf_mode = VPX_KF_DISABLED;
    cfg.kf_min_dist = 9999;
    cfg.kf_max_dist = 9999;
    cfg.g_threads = config_.threads > 0 ? static_cast<unsigned int>(config_.threads)
                                        : 4u;
    if (vpx_codec_enc_init(&ctx_, iface, &cfg, 0) != VPX_CODEC_OK) {
      error = std::string("vpx: init failed: ") +
              (vpx_codec_error(&ctx_) != nullptr ? vpx_codec_error(&ctx_)
                                                 : "unknown");
      return false;
    }
    // Fastest realtime speed; the capture's QP controller owns the quality
    // trade, not the encoder's speed setting.
    vpx_codec_control(&ctx_, VP8E_SET_CPUUSED, 8);
    // Fixed quality: the same qp the AV1/H.264 streams use.
    vpx_codec_control(&ctx_, VP8E_SET_CQ_LEVEL,
                      static_cast<unsigned int>(qp));
    vpx_codec_control(&ctx_, VP8E_SET_STATIC_THRESHOLD, 0);
    vpx_codec_control(&ctx_, VP8E_SET_NOISE_SENSITIVITY, 0);
    if (codec_ == video_codec::vp9_stream) {
      vpx_codec_control(&ctx_, VP9E_SET_TILE_COLUMNS, 2);
      vpx_codec_control(&ctx_, VP9E_SET_ROW_MT, 1);
      vpx_codec_control(&ctx_, VP9E_SET_COLOR_SPACE, VPX_CS_BT_709);
    }
    width_ = width;
    height_ = height;
    qp_ = qp;
    pts_ = 0;
    open_ = true;
    return true;
  }

  void destroy() {
    if (open_) {
      vpx_codec_destroy(&ctx_);
      open_ = false;
    }
    width_ = 0;
    height_ = 0;
    qp_ = 0;
  }

  av1_encoder_config config_;
  video_codec codec_;
  vpx_codec_ctx_t ctx_{};
  bool open_ = false;
  int width_ = 0;
  int height_ = 0;
  int qp_ = 0;
  int64_t pts_ = 0;
  std::vector<uint8_t> i420_;
};

}  // namespace

bool vpx_stream_available() { return true; }

std::shared_ptr<av1_stream_encoder> make_vpx_stream_encoder(
    const av1_encoder_config& config, video_codec codec, std::string* error) {
  if (codec != video_codec::vp9_stream && codec != video_codec::vp8_stream) {
    if (error != nullptr) *error = "vpx: not a VP8/VP9 stream";
    return nullptr;
  }
  return std::make_shared<vpx_stream_encoder>(config, codec);
}

}  // namespace molink

#else

namespace molink {

bool vpx_stream_available() { return false; }

std::shared_ptr<av1_stream_encoder> make_vpx_stream_encoder(
    const av1_encoder_config& config, video_codec codec, std::string* error) {
  (void)config;
  (void)codec;
  if (error != nullptr) *error = "this build has no VP8/VP9 encoder (libvpx)";
  return nullptr;
}

}  // namespace molink

#endif  // MOLINK_HAVE_VPX
