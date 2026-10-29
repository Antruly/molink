#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "session_controller.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_set>
#include <vector>

#include <web/uvcpp_ws_connection.h>
#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_json.h>

#include "config.h"
#include "capture/frame_encode.h"
#include "compression.h"
#include "capture/replay_policy.h"
#include "capture/video_push.h"
#if MOLINK_HAVE_WEBRTC
#include "webrtc/rtc_session.h"
#endif
#include "protocol/frame_packet.h"
#include "service_control.h"
#include "transport/ws_transport.h"
#include "util.h"
#include "version.h"

namespace molink {
namespace {

using uvcpp::uvcpp_json;
using uvcpp::uvcpp_json_dump;

namespace {

struct clipboard_sync_state {
  std::mutex mutex;
  std::unordered_set<std::string> seen;
  std::deque<std::string> order;
  std::atomic<bool> fetch_pending{false};
  std::atomic<bool> files_pending{false};
};

clipboard_sync_state g_clipboard_sync;

std::string clipboard_hash(const std::string& text) {
  uint32_t hash = 2166136261u;
  for (unsigned char byte : text) {
    hash ^= byte;
    hash *= 16777619u;
  }
  char buffer[9] = {};
  std::snprintf(buffer, sizeof(buffer), "%08x", hash);
  return buffer;
}

/// Returns true the first time a hash is seen; the bounded set keeps the
/// echo suppression window small (the last 256 clipboard values).
bool clipboard_take_hash(const std::string& hash) {
  std::lock_guard<std::mutex> lock(g_clipboard_sync.mutex);
  if (g_clipboard_sync.seen.count(hash) != 0) return false;
  g_clipboard_sync.seen.insert(hash);
  g_clipboard_sync.order.push_back(hash);
  while (g_clipboard_sync.order.size() > 256) {
    g_clipboard_sync.seen.erase(g_clipboard_sync.order.front());
    g_clipboard_sync.order.pop_front();
  }
  return true;
}

}  // namespace

uint64_t epoch_us() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<microseconds>(system_clock::now().time_since_epoch())
          .count());
}

/// A display mode change resets the driver and the desktop duplication. A
/// burst of requests (someone clicking through the resolution menu) can leave
/// the adapter in a bad state, so the host answers at most one change per
/// couple of seconds. The display is shared by every session.
constexpr uint64_t kDisplayModeCooldownMs = 2000;
std::atomic<uint64_t> g_last_display_mode_ms{0};

const char* codec_name(uint8_t codec) {
  switch (static_cast<protocol::codec>(codec)) {
    case protocol::codec::deflate_bgra:
      return "deflate-bgra";
    case protocol::codec::rgb24:
      return "rgb24";
    case protocol::codec::rgb565:
      return "rgb565";
    case protocol::codec::deflate_rgb24:
      return "deflate-rgb24";
    case protocol::codec::deflate_rgb565:
      return "deflate-rgb565";
    case protocol::codec::png:
      return "png";
    case protocol::codec::h264:
      return "h264";
    case protocol::codec::av1:
      return "av1";
    case protocol::codec::bgra:
    default:
      return "bgra";
  }
}

std::string string_field(const uvcpp_json& j, const char* key) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_string()) return std::string();
  return it->get<std::string>();
}

void send_json(session_transport* transport, const uvcpp_json& j) {
  if (transport == nullptr || !transport->is_open()) return;
  const std::string body = uvcpp_json_dump(j);
  transport->send_text(transport_channel::control, body);
}

void send_error(session_transport* transport, const char* code,
                const std::string& message) {
  uvcpp_json j;
  j["type"] = "error";
  j["code"] = code;
  j["message"] = message;
  send_json(transport, j);
}

void send_pong(session_transport* transport, const uvcpp_json& request) {
  uvcpp_json j;
  j["type"] = "pong";
  const auto it = request.find("t");
  if (it != request.end() && it->is_number()) j["t"] = *it;
  j["server_ms"] = now_ms();
  send_json(transport, j);
}

bool number_field(const uvcpp_json& j, const char* key, double& out) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_number()) return false;
  out = it->get<double>();
  return true;
}

bool bool_field(const uvcpp_json& j, const char* key, bool fallback) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_boolean()) return fallback;
  return it->get<bool>();
}

input_injector::status inject_one(input_injector& input, const uvcpp_json& event,
                                  std::string& err) {
  if (!event.is_object()) {
    err = "input event must be an object";
    return input_injector::status::rejected;
  }
  const std::string kind = string_field(event, "k");

  if (kind == "sas") {
    // Ctrl+Alt+Del cannot be synthesized with SendInput; the SYSTEM service
    // generates it (SoftwareSASGeneration policy).
    std::string sas_error;
    if (!send_secure_attention(sas_error)) {
      err = sas_error;
      return input_injector::status::rejected;
    }
    return input_injector::status::ok;
  }

  if (kind == "move") {
    double x = 0;
    double y = 0;
    if (!number_field(event, "x", x) || !number_field(event, "y", y)) {
      err = "move needs normalized x and y";
      return input_injector::status::rejected;
    }
    return input.move(x, y);
  }
  if (kind == "button") {
    double button = 1;
    if (!number_field(event, "button", button)) {
      err = "button needs a button number";
      return input_injector::status::rejected;
    }
    return input.button(static_cast<int>(button),
                        bool_field(event, "down", true));
  }
  if (kind == "wheel") {
    double dx = 0;
    double dy = 0;
    number_field(event, "dx", dx);
    number_field(event, "dy", dy);
    return input.wheel(static_cast<int>(dx), static_cast<int>(dy));
  }
  if (kind == "key") {
    const std::string code = string_field(event, "code");
    if (code.empty()) {
      err = "key needs a code";
      return input_injector::status::rejected;
    }
    return input.key(code, bool_field(event, "down", true));
  }
  if (kind == "text") {
    const std::string value = string_field(event, "text");
    if (value.empty() || value.size() > 4096) {
      err = "text must be 1..4096 bytes";
      return input_injector::status::rejected;
    }
    return input.text(value);
  }

  err = "unknown input event kind: " + kind;
  return input_injector::status::rejected;
}

bool send_frame_payload(session_transport* transport,
                        const std::shared_ptr<session_info>& session,
                        uint16_t flags, uint64_t ts_us,
                        const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> bytes;
  std::string err;
  const uint32_t seq = session->out_seq++;
  if (!protocol::encode_packet_into(
          bytes, static_cast<uint8_t>(protocol::packet_type::frame), flags,
          session->id, seq, ts_us, payload.data(), payload.size(), err)) {
    send_error(transport, "frame_build_failed", err);
    return false;
  }
  if (!transport->send_binary(transport_channel::video, bytes.data(),
                              bytes.size())) {
    send_error(transport, "send_failed", "could not queue frame");
    return false;
  }
  ++session->frames_sent;
  return true;
}

/// Sends one captured frame.
///
/// In full-frame AV1 mode the captured picture is the AV1 payload alone; the
/// base tile payload is empty and is not sent, so the wire never carries the
/// same image twice. In the older split mode the tile frame goes first and the
/// AV1 patch follows as a tile refresh over the image just painted.
///
/// Returns false when the frame could not be given to this session at all
/// (an AV1-only picture for a viewer that cannot decode AV1).
/// The codec of a complete FRAME payload (the frame header's codec byte).
uint8_t frame_payload_codec(const std::vector<uint8_t>& payload) {
  const std::size_t offset = 6;
  if (payload.size() <= offset) return 0;
  return payload[offset];
}

/// True when this session can decode the stream picture in @p payload. The
/// stream is only produced for a viewer set that can all decode it, but a
/// session can join mid-stream without the capability, and the supervisor
/// needs a second to notice.
bool can_decode_stream(const std::shared_ptr<session_info>& session,
                       const std::vector<uint8_t>& payload) {
  if (payload.empty()) return false;
  switch (frame_payload_codec(payload)) {
    case static_cast<uint8_t>(protocol::codec::h264_stream):
      return session->h264_stream;
    case static_cast<uint8_t>(protocol::codec::vp9_stream):
      return session->vp9_stream;
    case static_cast<uint8_t>(protocol::codec::vp8_stream):
      return session->vp8_stream;
    default:
      // AV1 stream.
      return session->av1;
  }
}

/// Audio and voice take the realtime channel when the session has one: the
/// WebRTC audio channel is unreliable and unordered, so a packet never queues
/// behind video, file bytes or retransmissions.
bool send_realtime_audio(const std::weak_ptr<session_info>& weak_session,
                         const std::vector<uint8_t>& packet) {
#if MOLINK_HAVE_WEBRTC
  const std::shared_ptr<session_info> session = weak_session.lock();
  if (session && session->rtc && session->rtc->audio_open()) {
    return session->rtc->send_audio(packet.data(), packet.size());
  }
#else
  (void)weak_session;
  (void)packet;
#endif
  return false;
}

/// File chunks take the dedicated reliable channel when the session has one:
/// its own SCTP stream, so a transfer never occupies the control queue.
bool send_realtime_file(const std::weak_ptr<session_info>& weak_session,
                        const std::vector<uint8_t>& packet) {
#if MOLINK_HAVE_WEBRTC
  const std::shared_ptr<session_info> session = weak_session.lock();
  if (session && session->rtc && session->rtc->file_open()) {
    return session->rtc->send_file(packet.data(), packet.size());
  }
#else
  (void)weak_session;
  (void)packet;
#endif
  return false;
}

bool send_frame_with_av1(session_transport* transport,
                         const std::shared_ptr<session_info>& session,
                         const captured_frame& frame) {
  if (frame.av1_only) {
    if (!can_decode_stream(session, frame.av1_payload)) return false;
#if MOLINK_HAVE_WEBRTC
    // WebRTC path: the H.264 stream goes out as RTP on the video track, which
    // the browser decodes natively. The MOLK packet stays off the wire, so the
    // same picture is not paid for twice.
    if (session->rtc && session->rtc->video_open() &&
        frame_payload_codec(frame.av1_payload) ==
            static_cast<uint8_t>(protocol::codec::h264_stream)) {
      protocol::parsed_frame parsed;
      std::string parse_error;
      if (protocol::parse_frame_payload(frame.av1_payload.data(),
                                        frame.av1_payload.size(), parsed,
                                        parse_error) &&
          !parsed.tiles.empty()) {
        const protocol::tile_view& view = parsed.tiles.front();
        if (session->rtc->send_video(view.data, view.size, frame.ts_us)) {
          ++session->frames_sent;
          return true;
        }
      }
    }
#endif
    return send_frame_payload(
        transport, session,
        frame.keyframe ? protocol::kPacketFlagKeyframe : 0, frame.ts_us,
        frame.av1_payload);
  }
  if (!send_frame_payload(transport, session,
                          frame.keyframe ? protocol::kPacketFlagKeyframe : 0,
                          frame.ts_us, frame.payload)) {
    return false;
  }
  if (session->av1 && !frame.av1_payload.empty()) {
    send_frame_payload(transport, session, protocol::kPacketFlagTileRefresh,
                       frame.ts_us, frame.av1_payload);
  }
  return true;
}

/// A frame with no tiles. It still advances the wire sequence, which tells the
/// client "nothing changed" without leaving its request in flight forever.
void send_empty_frame(session_transport* transport,
                      const std::shared_ptr<session_info>& session,
                      uint16_t width, uint16_t height, bool keyframe) {
  protocol::frame_header header;
  // The capture sequence this session is up to, not the wire sequence: an
  // empty frame says "state unchanged at capture N".
  header.frame_seq =
      session->last_capture_seq >= 0
          ? static_cast<uint32_t>(session->last_capture_seq)
          : 0;
  header.codec = static_cast<uint8_t>(protocol::codec::bgra);
  header.flags = keyframe ? protocol::kFrameFlagKeyframe
                          : protocol::kFrameFlagPartial;
  header.screen_w = width;
  header.screen_h = height;
  header.tile_w = 0;
  header.tile_h = 0;

  std::vector<uint8_t> payload;
  std::string err;
  if (!protocol::build_frame_payload(header, {}, payload, err)) {
    send_error(transport, "frame_build_failed", err);
    return;
  }
  send_frame_payload(transport, session,
                     keyframe ? protocol::kPacketFlagKeyframe : 0, epoch_us(),
                     payload);
}


struct refresh_rect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

/// Answer a tile-level refresh from the current pixels. This replaces the
/// named rectangles without sending a full keyframe, which is what a bad link
/// needs: it is the same size as the damaged area and does not reset the whole
/// picture.
bool send_tile_refresh(session_transport* transport,
                       const std::shared_ptr<session_info>& session,
                       const session_options& opt,
                       const std::vector<refresh_rect>& rects,
                       std::string& err) {
  if (!opt.capture || rects.empty()) {
    err = "no refresh requested";
    return false;
  }
  std::vector<uint8_t> pixels;
  int width = 0;
  int height = 0;
  if (!opt.capture->copy_pixels(pixels, width, height) || width <= 0 ||
      height <= 0 ||
      pixels.size() != static_cast<std::size_t>(width) * height * 4) {
    err = "no pixel snapshot";
    return false;
  }
  std::vector<protocol::tile_blob> tiles;
  for (const refresh_rect& rect : rects) {
    if (tiles.size() >= 64) break;
    const int x = std::clamp(rect.x, 0, width - 1);
    const int y = std::clamp(rect.y, 0, height - 1);
    const int w = std::min(rect.w, width - x);
    const int h = std::min(rect.h, height - y);
    if (w <= 0 || h <= 0) continue;
    protocol::tile_blob tile;
    tile.x = static_cast<uint16_t>(x);
    tile.y = static_cast<uint16_t>(y);
    tile.w = static_cast<uint16_t>(w);
    tile.h = static_cast<uint16_t>(h);
    tile.data.resize(static_cast<std::size_t>(w) * h * 4);
    for (int row = 0; row < h; ++row) {
      std::memcpy(tile.data.data() + static_cast<std::size_t>(row) * w * 4,
                  pixels.data() +
                      (static_cast<std::size_t>(y + row) * width + x) * 4,
                  static_cast<std::size_t>(w) * 4);
    }
    tiles.push_back(std::move(tile));
  }
  if (tiles.empty()) {
    err = "refresh rectangles were empty";
    return false;
  }
  protocol::frame_header header;
  header.frame_seq = 0;
  header.codec = static_cast<uint8_t>(protocol::codec::bgra);
  header.flags = protocol::kFrameFlagPartial;
  header.screen_w = static_cast<uint16_t>(width);
  header.screen_h = static_cast<uint16_t>(height);
  header.tile_w = 128;
  header.tile_h = 128;
  encode_options encoding;
  encoding.compress = true;
  // A refresh is a patch of the live picture: it has to be produced in the
  // preset's own format, or a monochrome session gets colour patches painted
  // over it (the old hard-coded BGRA did exactly that in turbo).
  encoding.format = opt.capture->frame_format();
  encoding.effort = compression_effort::fast;
  std::vector<uint8_t> payload;
  // Tile refreshes are already a patch; never split them into AV1 here.
  if (!encode_capture_frame(header, tiles, encoding, nullptr, 0, payload,
                            nullptr, err)) {
    return false;
  }
  return send_frame_payload(transport, session,
                            protocol::kPacketFlagTileRefresh, epoch_us(),
                            payload);
}

/// Cursor bitmap and position are sent alongside frames: they are small, change
/// independently of screen damage, and are not part of an X11 grab.
void maybe_send_cursor(session_transport* transport,
                       const std::shared_ptr<session_info>& session,
                       const session_options& opt) {
  if (!opt.capture) return;
  cursor_snapshot cursor;
  if (!opt.capture->cursor(cursor)) return;

  const bool visible = cursor.width > 0 && cursor.height > 0;
  if (!visible) {
    if (session->cursor_visible) {
      uvcpp_json j;
      j["type"] = "cursor-position";
      j["visible"] = false;
      send_json(transport, j);
      session->cursor_visible = false;
    }
    return;
  }

  if (!session->cursor_has_shape || cursor.serial != session->cursor_serial) {
    uvcpp_json j;
    j["type"] = "cursor";
    j["serial"] = cursor.serial;
    j["w"] = cursor.width;
    j["h"] = cursor.height;
    j["hotspotX"] = cursor.hotspot_x;
    j["hotspotY"] = cursor.hotspot_y;
    j["rgba"] = base64_encode(cursor.rgba.data(), cursor.rgba.size());
    send_json(transport, j);
    session->cursor_serial = cursor.serial;
    session->cursor_has_shape = true;
    session->cursor_x = INT32_MIN;  // force a position after a shape change
  }

  if (cursor.x != session->cursor_x || cursor.y != session->cursor_y ||
      !session->cursor_visible) {
    uvcpp_json j;
    j["type"] = "cursor-position";
    j["x"] = cursor.x;
    j["y"] = cursor.y;
    j["visible"] = true;
    send_json(transport, j);
    session->cursor_x = cursor.x;
    session->cursor_y = cursor.y;
    session->cursor_visible = true;
  }
}

/// Sends whatever this session still needs to reach the newest capture.
///
/// @param push the host drives the clock for this session: when there is
///        nothing new, send nothing at all. A polling client has a request in
///        flight and still needs the empty frame to hear back; a pushed one
///        must not pay a packet for every idle tick.
void send_capture_frame(session_transport* transport,
                        const std::shared_ptr<session_info>& session,
                        const session_options& opt, bool push = false) {
  if (!opt.capture) {
    send_empty_frame(transport, session, 0, 0, false);
    return;
  }

  const std::shared_ptr<const captured_frame> latest = opt.capture->latest();
  if (!latest) {
    // Capture has not produced anything yet; let the client keep polling. A
    // pushed session has nothing in flight, so an empty frame every tick would
    // be pure noise until the first capture lands.
    if (!push) send_empty_frame(transport, session, 0, 0, false);
    return;
  }

  int64_t start_seq = -1;
  if (!session->has_image) {
    // The newest frame is usually a delta. Look back through the short
    // capture history for the keyframe instead of waiting for a fresh one;
    // otherwise a client that misses the exact keyframe moment keeps getting
    // empty frames.
    start_seq = latest->keyframe ? static_cast<int64_t>(latest->seq) : -1;
    if (start_seq < 0) {
      for (uint32_t back = 0;
           back < kReplayMaxFrames && latest->seq >= back; ++back) {
        const auto candidate = opt.capture->find(latest->seq - back);
        if (candidate && candidate->keyframe) {
          start_seq = candidate->seq;
          break;
        }
      }
    }
    if (start_seq < 0) {
      opt.capture->request_keyframe();
      if (!push) {
        send_empty_frame(transport, session, latest->width, latest->height,
                         false);
      }
      return;
    }
    session->last_capture_seq = start_seq - 1;
  }

  const int64_t last = session->last_capture_seq;
  if (last >= 0 && static_cast<uint32_t>(last) >= latest->seq) {
    // Nothing new since the last response. Answer anyway so the client's
    // request does not stay in flight -- unless the host owns the clock, in
    // which case an idle tick costs nothing.
    if (!push) {
      send_empty_frame(transport, session, latest->width, latest->height,
                       false);
    }
    return;
  }
  // A session without a picture replays from the keyframe it found above,
  // which may be frame 0. Treating last == -1 as "nothing to send" here
  // would leave a fresh session staring at empty frames forever.
  if (last < 0 && start_seq < 0) {
    send_empty_frame(transport, session, latest->width, latest->height, false);
    return;
  }

  // Replay every frame the session has not seen. Capture keeps a bounded
  // history precisely so a poll that arrived late does not need a keyframe.
  const uint32_t first = last < 0 ? static_cast<uint32_t>(start_seq)
                                  : static_cast<uint32_t>(last) + 1;
  if (first > latest->seq) {
    send_empty_frame(transport, session, latest->width, latest->height, false);
    return;
  }
  const uint32_t missing = latest->seq - first + 1;

  const uint64_t now_us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  const auto oldest_pending = opt.capture->find(first);
  const uint64_t oldest_age_us =
      oldest_pending && now_us > oldest_pending->ts_us
          ? now_us - oldest_pending->ts_us
          : 0;
  const replay_action action =
      choose_replay_action(missing, oldest_age_us, session->has_image);
  if (action == replay_action::skip_to_latest) {
    // Do not make a slow viewer replay a queue of obsolete deltas. Advance it
    // to the current capture sequence; the periodic tile clock repairs any
    // regions that were skipped.
    session->last_capture_seq = latest->seq;
    send_empty_frame(transport, session, latest->width, latest->height, false);
    return;
  }
  if (action == replay_action::request_keyframe) {
    session->last_capture_seq = -1;
    opt.capture->request_keyframe();
    send_empty_frame(transport, session, latest->width, latest->height, false);
    return;
  }


  for (uint32_t seq = first; seq <= latest->seq; ++seq) {
    const std::shared_ptr<const captured_frame> frame = opt.capture->find(seq);
    if (!frame) {
      // The replay history has evicted this frame. A viewer with a valid base
      // can skip to the latest capture and repair stale cells from tile_clock;
      // a new viewer still needs a keyframe to establish its base image.
      if (session->has_image) {
        session->last_capture_seq = latest->seq;
        send_empty_frame(transport, session, latest->width, latest->height,
                         false);
        return;
      }
      session->last_capture_seq = -1;
      opt.capture->request_keyframe();
      send_empty_frame(transport, session, latest->width, latest->height,
                       false);
      return;
    }
    if (now_us > frame->ts_us && now_us - frame->ts_us > 1000000ULL) {
      // The client missed this and everything after it. If it already has a
      // picture, patch just the missed rectangles from the current pixels;
      // that is far cheaper than a keyframe on a bad link. With no picture
      // yet, only a keyframe can bootstrap it.
      if (session->has_image) {
        std::vector<refresh_rect> rects;
        for (uint32_t s = static_cast<uint32_t>(last) + 1;
             s <= latest->seq && rects.size() < 64; ++s) {
          const auto missed = opt.capture->find(s);
          if (!missed) continue;
          // The base payload has the AV1 rectangles removed. Include the
          // patch's rectangles too, otherwise the cheap recovery skips
          // exactly the largest changed areas.
          auto collect = [&rects](const std::vector<uint8_t>& payload) {
            if (payload.empty()) return;
            protocol::parsed_frame parsed;
            std::string parse_error;
            if (!protocol::parse_frame_payload(payload.data(), payload.size(),
                                               parsed, parse_error)) {
              return;
            }
            for (const auto& tile : parsed.tiles) {
              if (rects.size() >= 64) break;
              rects.push_back({tile.x, tile.y, tile.w, tile.h});
            }
          };
          collect(missed->payload);
          collect(missed->av1_payload);
        }
        std::string refresh_error;
        if (send_tile_refresh(transport, session, opt, rects,
                              refresh_error)) {
          session->last_capture_seq = latest->seq;
          return;
        }
      }
      session->has_image = false;
      session->last_capture_seq = -1;
      opt.capture->request_keyframe();
      send_empty_frame(transport, session, latest->width, latest->height, false);
      return;
    }
    if (frame->av1_only && !can_decode_stream(session, frame->av1_payload)) {
      // A viewer that cannot decode AV1 joined while the capture was already
      // building AV1-only frames. Drop back to "no picture" so the next poll
      // replays a tile keyframe; the supervisor turns AV1 off for the next
      // capture because not every session supports it.
      session->has_image = false;
      session->last_capture_seq = -1;
      opt.capture->request_keyframe();
      send_empty_frame(transport, session, frame->width, frame->height, false);
      return;
    }
    if (!send_frame_with_av1(transport, session, *frame)) {
      // The transport refused the frame: its channel is full, or the session
      // is gone. Advancing anyway would tell the next tick that the viewer is
      // up to date when it is now one delta behind, and every later frame
      // would build on a picture it never applied. Start over from a keyframe
      // instead -- the sender's queue is part of the recovery loop.
      session->has_image = false;
      session->last_capture_seq = -1;
      opt.capture->request_keyframe();
      return;
    }
    session->last_capture_seq = seq;
    if (frame->keyframe) session->has_image = true;
  }
}

/// Sends the per-cell capture sequence map to a client that advertised
/// `seq_clock`. Compared with what the client received, it names the exact
/// cells a lost or skipped frame left stale, with no timestamp window and no
/// extra round trip. Throttled so a polling client does not flood the link
/// while the screen is static.
void send_tile_seq(session_transport* transport,
                   const std::shared_ptr<session_info>& session,
                   const session_options& opt) {
  if (!session->seq_clock || !session->has_image || !opt.capture) return;
  const uint64_t now_ms = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
  if (session->last_seq_ms != 0 && now_ms - session->last_seq_ms < 100) {
    return;
  }
  std::vector<uint8_t> map;
  if (!opt.capture->tile_seq(map)) return;
  session->last_seq_ms = now_ms;
  // Neighbouring cells usually share a sequence, so deflate shrinks the map
  // several-fold; the client inflates it before comparing. The whole map is
  // one packet on purpose: a client checks every block at once and asks only
  // for the ones it is missing.
  const std::vector<uint8_t>* payload = &map;
  std::vector<uint8_t> packed;
  uint16_t flags = 0;
  if (compression_available()) {
    std::string deflate_error;
    if (deflate_buffer(map.data(), map.size(), 1, packed, deflate_error) &&
        !packed.empty() && packed.size() < map.size()) {
      payload = &packed;
      flags = protocol::kPacketFlagCompressed;
    }
  }
  std::vector<uint8_t> bytes;
  std::string error;
  if (!protocol::encode_packet_into(
          bytes, static_cast<uint8_t>(protocol::packet_type::tile_seq), flags,
          session->id, 0, epoch_us(), payload->data(), payload->size(),
          error)) {
    return;
  }
  transport->send_binary(transport_channel::video, bytes.data(), bytes.size());
}

#if MOLINK_HAVE_WEBRTC
/// Sends one WebRTC signaling message to the browser over the session socket.
/// The signaling itself never moves to the data channel: it is what brings
/// that channel up.
static void post_rtc_signal(std::function<void(std::function<void()>)> post,
                     std::shared_ptr<session_registry> registry,
                     uint32_t id, const char* type, const std::string& sdp,
                     const std::string& candidate, const std::string& mid) {
  if (!post) return;
  post([registry, id, type = std::string(type), sdp, candidate, mid] {
    const std::shared_ptr<session_info> session = registry->find(id);
    if (!session || !session->transport) return;
    uvcpp_json j;
    if (type == "ice") {
      j["type"] = "rtc-ice";
      j["candidate"] = candidate;
      j["mid"] = mid;
    } else {
      j["type"] = "rtc-" + type;
      j["sdp"] = sdp;
    }
    send_json(session->transport.get(), j);
  });
}
#endif

void send_welcome(session_transport* transport, const session_info& session,
                  const session_options& opt,
                  const std::string& resume_token) {
  const std::shared_ptr<const captured_frame> frame =
      opt.capture ? opt.capture->latest() : nullptr;

  uvcpp_json j;
  j["type"] = "welcome";
  j["session"] = session.public_id;
  j["session_id"] = session.id;
  if (!resume_token.empty()) j["session_token"] = resume_token;
  j["protocol"] = protocol::kProtocolVersion;
  j["service"] = MOLINK_CORE_SERVICE;
  j["version"] = opt.version;
  // The viewer may report its own paint health; the host uses it to send less
  // detail when the client, not the encoder, is the limit.
  // The host resolves "auto" from the viewers' painted rate; an older
  // core needs the client to pick a concrete tier instead.
  j["auto_quality"] = true;
  j["viewer_stats"] = true;
  // The host can drive the frame clock itself. A client that advertises
  // `video_push` in hello stops polling `request-frame`; clients that never
  // ask keep the pull path, which stays the compatibility default.
  j["video_push"] = true;
  // The host can encode a low-delay H.264 stream on the GPU as well. A viewer
  // that advertises `h264_stream` in hello gets one when not every viewer can
  // decode AV1; clients that do not ask never see it.
  j["h264_stream"] = true;
  // Which low-delay stream codecs this host can actually encode right now.
  // The page offers only these in the session toolbar (intersected with what
  // it can decode); "auto" keeps the hardware-first order.
  {
    bool av1 = false;
    bool h264 = false;
    bool vp9 = false;
    bool vp8 = false;
    for (const video_encoder_backend& backend : video_encoder_backends()) {
      if (!backend.available) continue;
      av1 = av1 || backend.av1_stream;
      h264 = h264 || backend.h264_stream;
      vp9 = vp9 || backend.vp9_stream;
      vp8 = vp8 || backend.vp8_stream;
    }
    // Only what this viewer can actually decode as well: the toolbar must not
    // offer a codec the host can encode but the viewer cannot take. (AV1 goes
    // through the local core's proxy, so `av1_stream` is the proxy's answer.)
    j["stream_codecs"] = uvcpp_json::array();
    if (av1 && session.av1_stream) j["stream_codecs"].push_back("av1");
    if (h264 && session.h264_stream) j["stream_codecs"].push_back("h264");
    if (vp9 && session.vp9_stream) j["stream_codecs"].push_back("vp9");
    if (vp8 && session.vp8_stream) j["stream_codecs"].push_back("vp8");
  }
  j["codec_preference"] =
      opt.auth ? opt.auth->settings().stream_codec : std::string("auto");
  // WebRTC (Phase 4): the host can offer an H.264 video track the browser
  // decodes natively. Only advertised by builds that carry the media stack.
  j["webrtc"] = MOLINK_HAVE_WEBRTC ? true : false;
#if MOLINK_HAVE_WEBTRANSPORT
  // The browser opens a WebTransport session on its own port, pinning the
  // short-lived certificate by hash instead of trusting a CA.
  if (opt.webtransport_available && opt.webtransport_available->load() &&
      opt.network && !opt.webtransport_fingerprint.empty()) {
    j["webtransport"]["port"] = opt.network->webtransport_port.load();
    j["webtransport"]["sha256"] = opt.webtransport_fingerprint;
  }
#endif
  // The browser builds its RTCPeerConnection with the same ICE servers the
  // host's peer uses, so both sides gather on the same paths.
  j["ice_servers"] = uvcpp_json::array();
  if (const char* stun = std::getenv("MOLINK_STUN_HOST");
      stun != nullptr && *stun != '\0') {
    j["ice_servers"].push_back(std::string("stun:") + stun + ":3478");
  }
  if (const char* turn = std::getenv("MOLINK_TURN_URL");
      turn != nullptr && *turn != '\0') {
    j["ice_servers"].push_back(turn);
  }
  // `transport` stays the wire protocol for older clients; `transport_path`
  // is the orthogonal axis (direct / p2p / relay), so a relayed QUIC session
  // reads as path=relay, protocol=quic.
  j["transport"] = transport->name();
  j["transport_path"] = transport->path();
  // The host's alternatives, not just the protocol this session happened to
  // use: a QUIC session still has the WebSocket door for older clients.
  j["transports"] = uvcpp_json::array();
  j["transports"].push_back("websocket");
  if (opt.quic_available && opt.quic_available->load()) {
    j["transports"].push_back("quic");
  }
  if (opt.tcp_available && opt.tcp_available->load()) {
    j["transports"].push_back("tcp");
  }
  if (opt.http3_available && opt.http3_available->load()) {
    j["transports"].push_back("http3");
  }
  if (opt.http2_available) {
    j["transports"].push_back("http2");
  }
  const int web_port = opt.bound_port ? opt.bound_port->load() : 0;
  j["transport_ports"]["web"] = web_port;
  if (opt.network) {
    j["transport_ports"]["http3"] = opt.network->http3_port.load();
    j["transport_ports"]["quic"] = opt.network->quic_port.load();
    j["transport_ports"]["tcp_data"] = opt.network->tcp_data_port.load();
    j["transport_ports"]["udp_data"] = opt.network->udp_port.load();
  }
  if (opt.capture) {
    const std::vector<display_info> displays = opt.capture->displays();
    if (!displays.empty()) {
      j["displays"] = uvcpp_json::array();
      for (const display_info& display : displays) {
        uvcpp_json item;
        item["index"] = display.index;
        item["name"] = display.name;
        item["x"] = display.x;
        item["y"] = display.y;
        item["width"] = display.width;
        item["height"] = display.height;
        item["primary"] = display.primary;
        j["displays"].push_back(std::move(item));
      }
      j["display_index"] = opt.capture->current_display();
      const display_info current = opt.capture->current_display_info();
      if (opt.input && current.input_width > 0 && current.input_height > 0) {
        opt.input->set_display_rect(current.x, current.y, current.input_width,
                                    current.input_height);
      }
    }
  }
  j["tiles"]["codec"] = frame ? codec_name(frame->codec) : "bgra";
  j["tiles"]["quality"] =
      opt.capture ? quality_mode_name(opt.capture->quality()) : "balanced";
  j["tiles"]["screen_w"] = frame ? frame->width : 0;
  j["tiles"]["screen_h"] = frame ? frame->height : 0;
  j["tiles"]["tile_w"] = frame ? frame->tile_w : 0;
  j["tiles"]["tile_h"] = frame ? frame->tile_h : 0;
  j["tiles"]["tile_size"] = opt.tile_size;
  j["tiles"]["backend"] = opt.capture ? opt.capture->backend() : "none";
  j["tiles"]["synthetic"] = opt.capture ? !opt.capture->is_real() : true;
  // Clients use this to offer the resolution menu only where it can work.
  j["tiles"]["can_set_display_mode"] =
      opt.capture && opt.capture->supports_display_mode();
  j["input"] = opt.input != nullptr && opt.input->available();
  j["input_backend"] = opt.input ? opt.input->backend() : "none";
  j["call"] = opt.voice != nullptr;
  if (opt.audio && opt.audio->available()) {
    const audio_format format = opt.audio->format();
    j["audio"] = true;
    j["audio_format"]["sample_rate"] = format.sample_rate;
    j["audio_format"]["channels"] = format.channels;
    j["audio_format"]["bits"] = format.bits;
  } else {
    j["audio"] = false;
  }
  j["clipboard"] = opt.clipboard != nullptr && opt.clipboard->available();
  j["clipboard_backend"] = opt.clipboard ? opt.clipboard->backend() : "none";
  j["device_id"] = opt.device_id;
  if (!opt.cert_fingerprint.empty()) {
    j["tls_fingerprint"] = opt.cert_fingerprint;
  }
  // e2e: direct TLS to this host's certificate (WSS, or QUIC which is always
  // TLS); relay: the directory's outer TLS only; none: plain direct/LAN or raw
  // P2P control.
  const std::string transport_name = transport->name();
  const std::string transport_path = transport->path();
  std::string security = "none";
  if (transport_path == "relay") {
    security = "relay";
  } else if (transport_name == "quic" || transport_name == "http3" ||
             transport_name == "tcp" ||
             (opt.tls_enabled && transport_name == "websocket")) {
    security = "e2e";
  }
  j["security"] = security;
  std::string welcome_name = opt.device_name;
  if (opt.auth) {
    const app_settings current = opt.auth->settings();
    if (!current.device_name.empty()) welcome_name = current.device_name;
  }
  j["device_name"] = welcome_name;
  send_json(transport, j);
}

}  // namespace

session_controller::session_controller(
    std::shared_ptr<session_registry> registry,
    std::shared_ptr<session_transport> transport, session_options options)
    : registry_(std::move(registry)),
      transport_(transport),
      options_(std::move(options)) {}

bool session_controller::start(const std::string* initial_token) {
  std::shared_ptr<session_transport> transport = transport_.lock();
  if (!transport) return false;
  const std::string peer_ip = transport->peer_ip();

  if (options_.lock) {
    const int delay = options_.lock->attempt_delay(peer_ip);
    if (delay != 0) {
      const bool banned = delay < 0;
      send_error(transport.get(), banned ? "ip_banned" : "too_many_attempts",
                 banned ? "this address is blocked"
                        : "too many failed attempts; wait before trying again");
      transport->close(transport_close::policy,
                       banned ? "ip banned" : "too many attempts");
      return false;
    }
  }

  const std::shared_ptr<session_info> session = registry_->add(transport);
  session_ = session;
  session->controller = shared_from_this();

  // Install handlers before the first message so a fast client cannot race its
  // hello. The weak capture keeps the controller from owning its transport.
  std::weak_ptr<session_controller> weak = weak_from_this();
  files_ = std::make_unique<file_transfer>(
      [weak](std::string text) {
        if (auto self = weak.lock()) self->send_json_string(text);
      },
      [weak](uint32_t id, uint64_t offset, const uint8_t* data,
             std::size_t len) {
        if (auto self = weak.lock()) {
          return self->send_file_chunk(id, offset, data, len);
        }
        return false;
      });
  transport->set_handlers(
      [weak](const std::string& text) {
        if (auto self = weak.lock()) self->on_text(text);
      },
      [weak](const uint8_t* data, std::size_t len) {
        if (auto self = weak.lock()) self->on_binary(data, len);
      },
      [weak](int code, const std::string& reason) {
        if (auto self = weak.lock()) self->on_close(code, reason);
      });

  if (initial_token != nullptr) {
    // A locked core leaves the session unauthenticated; the hello handler
    // reports "locked" once the connection is fully established, where the
    // error and close frame are actually delivered.
    const bool locked = options_.lock && options_.lock->locked() &&
                        !is_loopback_peer(peer_ip);
    if (!locked && options_.auth && options_.auth->authenticate(
                                         *initial_token, /*consume=*/true)) {
      registry_->set_authenticated(session);
      if (options_.auth->valid_session(*initial_token)) {
        resume_token_ = *initial_token;
      } else {
        issue_resume_token();
      }
      if (options_.lock) options_.lock->record_success(peer_ip);
    } else if (!locked) {
      if (options_.lock) {
        const int delay = options_.lock->record_failure(peer_ip);
        if (delay != 0) {
          const bool banned = delay < 0;
          send_error(transport.get(),
                     banned ? "ip_banned" : "too_many_attempts",
                     banned ? "this address is blocked"
                            : "too many failed attempts; wait before trying again");
          transport->close(transport_close::policy,
                           banned ? "ip banned" : "too many attempts");
          return false;
        }
      }
      send_error(transport.get(), "unauthorized", "invalid token");
      transport->close(transport_close::policy, "invalid token");
      return false;
    }
  }

  if (session->authenticated && options_.capture) {
    options_.capture->set_active(true);
  }
  if (session->authenticated) {
    send_welcome_message();
  } else {
    uvcpp_json j;
    j["type"] = "hello_required";
    j["protocol"] = protocol::kProtocolVersion;
    j["service"] = MOLINK_CORE_SERVICE;
    j["version"] = options_.version;
    send_json(transport.get(), j);
  }
  return true;
}

void session_controller::send_welcome_message() {
  std::shared_ptr<session_transport> transport = transport_.lock();
  std::shared_ptr<session_info> session = session_.lock();
  if (transport && session) {
    send_welcome(transport.get(), *session, options_, resume_token_);
    attach_chat();
    attach_call();
  }
}

void session_controller::issue_resume_token() {
  if (!resume_token_.empty()) return;
  if (options_.auth) resume_token_ = options_.auth->create_session();
}

void session_controller::attach_call() {
  if (!options_.call || call_subscription_ != 0) return;
  std::weak_ptr<session_controller> weak = weak_from_this();
  call_subscription_ = options_.call->subscribe(
      [weak](const call_snapshot& snapshot, const std::string& event) {
        if (auto self = weak.lock()) self->on_call_event(snapshot, event);
      });
}

void session_controller::on_call_event(const call_snapshot& snapshot,
                                       const std::string& event) {
  // The host accepts/declines through /api/call, so this runs on an HTTP
  // worker thread. The voice engine, its WASAPI objects and the websocket
  // belong to the app loop; marshal before touching any of them.
  const std::weak_ptr<session_controller> weak = weak_from_this();
  auto deliver = [weak, snapshot, event] {
    if (auto self = weak.lock()) self->apply_call_event(snapshot, event);
  };
  if (options_.post) {
    options_.post(std::move(deliver));
  } else {
    deliver();
  }
}

void session_controller::apply_call_event(const call_snapshot& snapshot,
                                          const std::string& event) {
  std::shared_ptr<session_info> session = session_.lock();
  if (!session) return;
  // hangup()/decline()/end() clear the snapshot's session id before they
  // notify, so an owner check would swallow the event: the host microphone
  // would keep running after the caller hung up, and other viewers would
  // never learn the call ended. These two events go to every session.
  const bool global_event = event == "ended" || event == "declined";
  if (!global_event && snapshot.session_id != session->id) return;
  uvcpp_json j;
  j["type"] = "call";
  j["action"] = event;
  j["host_microphone"] = snapshot.host_microphone;
  j["started_ms"] = snapshot.started_ms;
  send_json_string(uvcpp_json_dump(j));
  if (event == "accepted") {
    start_voice();
  } else if (event == "ended" || event == "declined") {
    stop_voice();
  }
}

void session_controller::start_voice() {
  if (voice_active_ || !options_.voice) return;
  std::weak_ptr<session_controller> weak = weak_from_this();
  auto post = options_.post;
  std::string error;
  voice_active_ = options_.voice->start(
      [weak, post](std::vector<uint8_t> pcm) {
        auto deliver = [weak, pcm = std::move(pcm)]() mutable {
          if (auto self = weak.lock()) self->send_voice(std::move(pcm));
        };
        if (post) {
          post(std::move(deliver));
        } else {
          deliver();
        }
      },
      error);
  if (!voice_active_) {
    uvcpp_json j;
    j["type"] = "call";
    j["action"] = "audio_error";
    j["message"] = error;
    send_json_string(uvcpp_json_dump(j));
  }
}

void session_controller::stop_voice() {
  if (!voice_active_) return;
  if (options_.voice) options_.voice->stop();
  voice_active_ = false;
}

void session_controller::send_voice(std::vector<uint8_t> pcm) {
  std::shared_ptr<session_transport> transport = transport_.lock();
  if (!transport || !transport->is_open() || pcm.empty()) return;
  std::vector<uint8_t> out;
  std::string error;
  if (protocol::encode_packet_into(
          out, static_cast<uint8_t>(protocol::packet_type::voice), 0, 0, 0,
          epoch_us(), pcm.data(), pcm.size(), error)) {
    if (send_realtime_audio(session_, out)) return;
    transport->send_binary(transport_channel::audio, out.data(), out.size());
  }
}

void session_controller::start_rtc(const std::shared_ptr<session_info>& session) {
#if MOLINK_HAVE_WEBRTC
  if (!session || session->rtc) return;
  std::shared_ptr<session_transport> transport = transport_.lock();
  if (!transport) return;

  rtc_session::config config;
  // Field setup: MOLINK_STUN_HOST is the same variable the native P2P path
  // uses, so one environment configures both. TURN is opt-in.
  if (const char* stun = std::getenv("MOLINK_STUN_HOST");
      stun != nullptr && *stun != '\0') {
    config.ice_servers.push_back(std::string("stun:") + stun + ":3478");
  }
  if (const char* turn = std::getenv("MOLINK_TURN_URL");
      turn != nullptr && *turn != '\0') {
    config.ice_servers.push_back(turn);
  }

  auto rtc = std::make_shared<rtc_session>();
  session->rtc = rtc;
  const uint32_t id = session->id;
  auto registry_ref = registry_;
  auto post = options_.post;
  std::weak_ptr<session_controller> weak = weak_from_this();

  rtc_session::callbacks cb;
  cb.on_local_description = [post, registry_ref, id](const std::string& type,
                                                     const std::string& sdp) {
    post_rtc_signal(post, registry_ref, id, type.c_str(), sdp, "", "");
  };
  cb.on_local_candidate = [post, registry_ref, id](
                              const std::string& candidate,
                              const std::string& mid) {
    post_rtc_signal(post, registry_ref, id, "ice", "", candidate, mid);
  };
  cb.on_state = [post, registry_ref, id](const char* state) {
    if (std::getenv("MOLINK_RTC_LOG") != nullptr) {
      std::fprintf(stderr, "webrtc[%u]: %s\n", id, state);
    }
    // A failed or closed peer keeps ICE sockets and a DTLS context alive
    // forever otherwise; the session falls back to the MOLK path.
    const std::string name(state);
    if (name != "failed" && name != "closed") return;
    if (!post) return;
    post([registry_ref, id] {
      if (auto session = registry_ref->find(id)) {
        if (session->rtc) {
          session->rtc->close();
          session->rtc.reset();
        }
      }
    });
  };
  // A live video track is a decoder: from now on this session can take the
  // H.264 stream even if it never advertised WebCodecs support.
  cb.on_data_channel_open = [post, registry_ref, id] {
    if (!post) return;
    post([registry_ref, id] {
      if (auto session = registry_ref->find(id)) session->h264_stream = true;
    });
  };
  cb.on_text = [weak, id](const std::string& text) {
    if (auto self = weak.lock()) {
      if (auto post = self->options_.post) {
        post([weak, id, text] {
          if (auto controller = weak.lock()) controller->on_text(text);
        });
      }
    }
  };
  cb.on_keyframe_request = [weak] {
    if (auto self = weak.lock()) {
      if (self->options_.capture) self->options_.capture->request_keyframe();
    }
  };
  cb.on_binary = [weak](const std::vector<uint8_t>& data) {
    if (auto self = weak.lock()) {
      if (auto post = self->options_.post) {
        post([weak, data] {
          if (auto controller = weak.lock()) {
            controller->on_binary(data.data(), data.size());
          }
        });
      }
    }
  };

  std::string error;
  if (!rtc->start(config, std::move(cb), &error)) {
    std::fprintf(stderr, "webrtc[%u]: %s\n", id, error.c_str());
    session->rtc.reset();
  }
#else
  (void)session;
#endif
}

void session_controller::send_json_string(const std::string& text) {
  std::shared_ptr<session_transport> transport = transport_.lock();
  if (transport && transport->is_open()) {
    transport->send_text(transport_channel::control, text);
  }
}

bool session_controller::send_file_chunk(uint32_t id, uint64_t offset,
                                         const uint8_t* data,
                                         std::size_t len) {
  std::shared_ptr<session_transport> transport = transport_.lock();
  if (!transport || !transport->is_open()) return false;
  std::vector<uint8_t> payload(16 + len);
  std::memcpy(payload.data(), &id, sizeof(id));
  std::memset(payload.data() + 4, 0, 4);
  std::memcpy(payload.data() + 8, &offset, sizeof(offset));
  if (len != 0) std::memcpy(payload.data() + 16, data, len);
  std::vector<uint8_t> out;
  std::string err;
  if (protocol::encode_packet_into(
          out, static_cast<uint8_t>(protocol::packet_type::file_data), 0, 0, 0,
          epoch_us(), payload.data(), payload.size(), err)) {
    if (send_realtime_file(session_, out)) return true;
    return transport->send_binary(transport_channel::file, out.data(), out.size());
  }
  return false;
}

void session_controller::send_audio(std::vector<uint8_t> pcm) {
  std::shared_ptr<session_transport> transport = transport_.lock();
  if (!transport || !transport->is_open() || pcm.empty()) return;
  std::vector<uint8_t> out;
  std::string err;
  if (protocol::encode_packet_into(
          out, static_cast<uint8_t>(protocol::packet_type::audio), 0, 0, 0,
          epoch_us(), pcm.data(), pcm.size(), err)) {
    if (send_realtime_audio(session_, out)) return;
    transport->send_binary(transport_channel::audio, out.data(), out.size());
  }
}

void session_controller::attach_chat() {
  if (!options_.chat || chat_subscription_ != 0) return;
  chat_history();
  std::weak_ptr<session_transport> transport = transport_;
  auto post = options_.post;
  chat_subscription_ = options_.chat->subscribe(
      [transport, post](const chat_message& message) {
        uvcpp_json item;
        item["type"] = "chat";
        item["action"] = "message";
        item["id"] = message.id;
        item["from"] = message.from;
        item["sender_name"] = message.sender_name;
        item["sender_id"] = message.sender_id;
        item["sender_session"] = message.sender_session;
        item["text"] = message.text;
        item["ts"] = message.ts_ms;
        const std::string body = uvcpp_json_dump(item);
        auto deliver = [transport, body] {
          if (auto open = transport.lock(); open && open->is_open()) {
            open->send_text(transport_channel::control, body);
          }
        };
        if (post) {
          post(std::move(deliver));
        } else {
          deliver();
        }
      });
}

void session_controller::chat_history() {
  if (!options_.chat) return;
  const std::vector<chat_message> history = options_.chat->history(0);
  uvcpp_json j;
  j["type"] = "chat";
  j["action"] = "history";
  uvcpp_json messages = uvcpp_json::array();
  for (const chat_message& message : history) {
    uvcpp_json item;
    item["id"] = message.id;
    item["from"] = message.from;
    item["sender_name"] = message.sender_name;
    item["sender_id"] = message.sender_id;
    item["sender_session"] = message.sender_session;
    item["text"] = message.text;
    item["ts"] = message.ts_ms;
    messages.push_back(std::move(item));
  }
  j["messages"] = std::move(messages);
  send_json_string(uvcpp_json_dump(j));
}

void session_controller::approve_access_request() {
  std::shared_ptr<session_transport> transport = transport_.lock();
  std::shared_ptr<session_info> session = session_.lock();
  if (!transport || !session) return;
  session->access_request_id = 0;
  registry_->set_awaiting_approval(session, false);
  registry_->set_authenticated(session);
  registry_->touch(*session);
  issue_resume_token();
  if (options_.capture) options_.capture->set_active(true);
  send_welcome_message();
}

void session_controller::deny_access_request() {
  std::shared_ptr<session_transport> transport = transport_.lock();
  std::shared_ptr<session_info> session = session_.lock();
  if (!transport || !session) return;
  session->access_request_id = 0;
  registry_->set_awaiting_approval(session, false);
  send_error(transport.get(), "access_denied",
             "the host declined the request");
  transport->close(transport_close::policy, "access denied");
}

void session_controller::on_text(const std::string& text) {
  std::shared_ptr<session_transport> transport = transport_.lock();
  std::shared_ptr<session_info> session = session_.lock();
  if (!transport || !session) return;
  const std::string peer_ip = transport->peer_ip();

  session->last_seen_ms = now_ms();
  if (options_.lock) options_.lock->touch(!is_loopback_peer(peer_ip));

  uvcpp_json j;
  const uvcpp::json_status status = uvcpp::uvcpp_json_parse(text, j);
  if (status != uvcpp::json_status::OK || !j.is_object()) {
    send_error(transport.get(), "bad_json",
               "control message must be a JSON object");
    return;
  }
  const std::string type = string_field(j, "type");

  auto require_auth = [&]() -> bool {
    if (session->authenticated) return true;
    send_error(transport.get(), "unauthorized",
               "send a hello message with a valid token");
    return false;
  };

  if (type == "hello") {
    // Optional client identity; the shared chat uses it to say who typed a
    // message when several people are connected.
    const std::string client_name = string_field(j, "name");
    const std::string client_id = string_field(j, "device_id");
    if (!client_name.empty()) session->client_name = client_name.substr(0, 64);
    if (!client_id.empty()) session->client_id = client_id.substr(0, 64);
    // A repeat hello must not cost another use of a multi-use code: the relay
    // proxy replays one when its first datagram races the session setup, and
    // a reconnect replays the whole handshake.
    if (session->authenticated) {
      send_welcome_message();
      return;
    }
    // A locked host still accepts a session that presents the verification
    // code: the lock asks for the code again, it is not a blanket refusal.
    // (Access requests without a code remain blocked below.)
    const bool locked_for_peer = options_.lock && options_.lock->locked() &&
                                 !is_loopback_peer(peer_ip);
    // `session` is the reconnect credential from a previous welcome. It is a
    // session token, not a code: checking it through valid_session() keeps it
    // from bypassing the temporary code's usage budget.
    //
    // While the host is locked the token is still honoured for a grace period.
    // A fresh token is a stronger credential than the code itself, and a
    // single-use code is already spent: refusing a viewer that lost the link a
    // minute ago left it with nothing to present, and every retry counted as a
    // failed code until the address was locked out.
    // A token stays usable for a week and is refreshed on every use, so a
    // viewer that was connected a moment ago can always return: after a host
    // restart (the tokens are persisted), after an idle lock, or after a long
    // session. A single-use code would be spent by then.
    constexpr uint64_t kResumeLifetimeMs = 7ull * 24ull * 60ull * 60ull * 1000ull;
    const std::string resume = string_field(j, "session");
    bool authenticated = false;
    if (!resume.empty() && options_.auth &&
        (locked_for_peer
             ? options_.auth->valid_recent_session(resume, kResumeLifetimeMs)
             : options_.auth->valid_session(resume))) {
      authenticated = true;
      resume_token_ = resume;
      options_.auth->touch_session(resume);
    } else if (options_.auth && options_.auth->authenticate(
                                   string_field(j, "token"),
                                   /*consume=*/true)) {
      authenticated = true;
      issue_resume_token();
    }
    if (!authenticated) {
      if (options_.lock) {
        const int delay = options_.lock->record_failure(peer_ip);
        if (delay != 0) {
          const bool banned = delay < 0;
          send_error(transport.get(),
                     banned ? "ip_banned" : "too_many_attempts",
                     banned ? "this address is blocked"
                            : "too many failed attempts; wait before trying again");
          transport->close(transport_close::policy,
                           banned ? "ip banned" : "too many attempts");
          return;
        }
      }
      send_error(transport.get(), "unauthorized", "invalid token");
      transport->close(transport_close::policy, "invalid token");
      return;
    }
    session->av1 = bool_field(j, "av1", false);
    session->av1_stream = bool_field(j, "av1_stream", false);
    // Clients that understand the per-block sequence map get one with every
    // frame response, so a lost frame is repaired immediately and precisely.
    session->seq_clock = bool_field(j, "seq_clock", false);
    session->video_push = bool_field(j, "video_push", false);
    session->h264_stream = bool_field(j, "h264_stream", false);
    session->vp9_stream = bool_field(j, "vp9_stream", false);
    session->vp8_stream = bool_field(j, "vp8_stream", false);
    session->webrtc = bool_field(j, "webrtc", false);
    if (std::getenv("MOLINK_AV1_LOG") != nullptr) {
      std::fprintf(stderr, "host: session av1=%d\n", session->av1 ? 1 : 0);
    }
    // MOLINK_STREAM_LOG=1: what this viewer can decode, so "the host sends
    // tiles" can be told apart from "the page never asked for a stream".
    if (std::getenv("MOLINK_STREAM_LOG") != nullptr) {
      std::fprintf(stderr,
                   "host: hello av1=%d av1_stream=%d h264_stream=%d webrtc=%d "
                   "video_push=%d vp9=%d vp8=%d\n",
                   session->av1 ? 1 : 0, session->av1_stream ? 1 : 0,
                   session->h264_stream ? 1 : 0, session->webrtc ? 1 : 0,
                   session->video_push ? 1 : 0, session->vp9_stream ? 1 : 0,
                   session->vp8_stream ? 1 : 0);
    }
    registry_->set_authenticated(session);
    // A session without AV1 must stop patches immediately; the supervisor
    // turns them back on once every session can decode them again.
    if (!session->av1 && options_.capture) options_.capture->set_av1_allowed(false);
    if (locked_for_peer) options_.lock->unlock();
    if (options_.lock) options_.lock->record_success(peer_ip);
    if (options_.capture) options_.capture->set_active(true);
    send_welcome_message();
    // The peer is built after the welcome so the browser already knows the
    // ICE servers and the host's capabilities when the offer arrives.
    if (session->webrtc) start_rtc(session);
    return;
  }

  if (type == "pong") {
    // Reply to the supervisor's heartbeat. There is nothing to answer.
    return;
  }

  if (type == "ping") {
    send_pong(transport.get(), j);
    return;
  }

  // No-code remote access: the local user gets a dialog and decides. The
  // session stays unauthenticated (and alive) until then.
  if (type == "request-access") {
    if (session->authenticated) {
      send_welcome_message();
      return;
    }
    if (!options_.access_requests) {
      send_error(transport.get(), "approval_unavailable",
                 "access requests are unavailable");
      return;
    }
    const app_settings settings =
        options_.auth ? options_.auth->settings() : app_settings{};
    if (!settings.allow_access_requests) {
      send_error(transport.get(), "approval_disabled",
                 "remote approval is disabled on this host");
      return;
    }
    if (options_.lock && options_.lock->locked() &&
        !is_loopback_peer(peer_ip)) {
      send_error(transport.get(), "locked", "the UI is locked");
      transport->close(transport_close::policy, "locked");
      return;
    }
    if (session->access_request_id != 0) {
      uvcpp_json ack;
      ack["type"] = "access-requested";
      ack["id"] = session->access_request_id;
      send_json(transport.get(), ack);
      return;
    }
    access_request_info info;
    info.peer_ip = peer_ip;
    info.name = string_field(j, "name").substr(0, 64);
    info.client = string_field(j, "client").substr(0, 32);
    info.created_ms = now_ms();
    session->client_name =
        string_field(j, "name").substr(0, 64);
    session->client_id =
        string_field(j, "device_id").substr(0, 64);
    const std::weak_ptr<session_controller> weak = weak_from_this();
    const uint32_t id = options_.access_requests->add(
        info,
        [weak] {
          if (auto self = weak.lock()) self->approve_access_request();
        },
        [weak] {
          if (auto self = weak.lock()) self->deny_access_request();
        });
    if (id == 0) {
      send_error(transport.get(), "too_many_requests",
                 "too many pending access requests");
      return;
    }
    session->access_request_id = id;
    registry_->set_awaiting_approval(session, true);
    uvcpp_json ack;
    ack["type"] = "access-requested";
    ack["id"] = id;
    send_json(transport.get(), ack);
    return;
  }

  if (!require_auth()) {
    transport->close(transport_close::policy, "not authenticated");
    return;
  }

  auto apply_fps = [&](int fps) {
    if (fps < 1) fps = 1;
    if (fps > 60) fps = 60;
    options_.capture->set_target_fps(fps);
    return fps;
  };

  if (type == "viewer") {
    // A client-side health sample: how many frames it actually painted, how
    // many it dropped, and how many bytes it received in the last second. The
    // supervisor turns this into back-pressure for the capture.
    double fps = 0;
    double dropped = 0;
    double rx = 0;
    number_field(j, "fps", fps);
    number_field(j, "dropped", dropped);
    number_field(j, "rx", rx);
    session->viewer_paint_fps = fps < 0 ? 0 : fps;
    session->viewer_dropped = dropped < 0 ? 0 : static_cast<int>(dropped);
    session->viewer_rx_rate = rx < 0 ? 0 : rx;
    session->viewer_stats_ms = now_ms();
    return;
  }

  if (type == "capture-fps") {
    if (!options_.capture) {
      send_error(transport.get(), "not_implemented", "capture is unavailable");
      return;
    }
    const auto fps_it = j.find("fps");
    if (fps_it == j.end() || !fps_it->is_number()) {
      send_error(transport.get(), "bad_fps", "fps must be a number");
      return;
    }
    const int fps = apply_fps(static_cast<int>(fps_it->get<double>()));
    uvcpp_json ack;
    ack["type"] = "capture-fps";
    ack["fps"] = fps;
    ack["scope"] = "global";
    send_json(transport.get(), ack);
    return;
  }

  // Quality preset plus an optional capture rate. The client resolves "auto"
  // into a concrete preset, so the core only has to apply encoding knobs.
  // Both are global to the host, like the capture rate.
  if (type == "quality") {
    if (!options_.capture) {
      send_error(transport.get(), "not_implemented", "capture is unavailable");
      return;
    }
    const std::string mode_name = string_field(j, "mode");
    quality_mode mode = options_.capture->quality();
    if (!mode_name.empty() && !quality_mode_from_name(mode_name, mode)) {
      send_error(transport.get(), "bad_quality",
                 "mode must be turbo, smooth, balanced, quality, ultra, lossless, raw or game");
      return;
    }
    const auto fps_it = j.find("fps");
    if (fps_it != j.end() && !fps_it->is_number()) {
      send_error(transport.get(), "bad_fps", "fps must be a number");
      return;
    }
    const quality_mode previous = options_.capture->quality();
    if (!mode_name.empty()) options_.capture->set_quality(mode);
    // A preset change is not just a knob: turbo encodes gray8, and the other
    // presets switch between BGRA/16-bit colour. Tiles the client already
    // holds keep the old format until they happen to change, so switching
    // turbo -> colour used to leave most of the screen monochrome until every
    // tile was touched. Push one full frame immediately after any change.
    if (options_.capture->quality() != previous) {
      options_.capture->request_keyframe();
      // The preset is global to the host. Every other viewer has to see the
      // same one, otherwise its UI keeps asserting its own choice and the
      // host ends up re-encoding for whoever pushed last.
      uvcpp_json shared;
      shared["type"] = "quality";
      shared["mode"] = quality_mode_name(options_.capture->quality());
      shared["format"] = tile_format_name(
          quality_profile_for(options_.capture->quality()).format);
      shared["scope"] = "global";
      for (const session_info& peer : registry_->snapshot()) {
        if (peer.id == session->id || !peer.authenticated ||
            !peer.transport) {
          continue;
        }
        send_json(peer.transport.get(), shared);
      }
    }
    int fps = 0;
    if (fps_it != j.end()) {
      fps = apply_fps(static_cast<int>(fps_it->get<double>()));
    }
    uvcpp_json ack;
    ack["type"] = "quality";
    ack["mode"] = quality_mode_name(options_.capture->quality());
    ack["format"] = tile_format_name(
        quality_profile_for(options_.capture->quality()).format);
    if (fps != 0) ack["fps"] = fps;
    ack["scope"] = "global";
    send_json(transport.get(), ack);
    return;
  }

  if (type == "display") {
    if (!options_.capture) {
      send_error(transport.get(), "not_implemented", "capture is unavailable");
      return;
    }
    const auto index_it = j.find("index");
    if (index_it == j.end() || !index_it->is_number()) {
      send_error(transport.get(), "bad_display", "index must be a number");
      return;
    }
    const int index = static_cast<int>(index_it->get<double>());
    const std::vector<display_info> displays = options_.capture->displays();
    if (index < 0 || index >= static_cast<int>(displays.size()) ||
        !options_.capture->select_display(index)) {
      send_error(transport.get(), "bad_display", "unknown monitor index");
      return;
    }
    if (options_.input) {
      const display_info& target = displays[index];
      options_.input->set_display_rect(target.x, target.y,
                                       target.input_width,
                                       target.input_height);
    }
    uvcpp_json ack;
    ack["type"] = "display";
    ack["index"] = index;
    send_json(transport.get(), ack);
    return;
  }

  // Real-time clipboard sync. The core applies the text to the host
  // clipboard and forwards it to every other authenticated session, keeping
  // the origin chain so nested sessions cannot echo it back.
  if (type == "clipboard-sync") {
    if (!require_auth()) {
      transport->close(transport_close::policy, "not authenticated");
      return;
    }
    const std::string text = string_field(j, "text");
    if (text.empty() || text.size() > 192 * 1024) return;
    const std::string hash = clipboard_hash(text);
    if (!clipboard_take_hash(hash)) return;

    std::string chain = string_field(j, "chain");
    const std::string& device = options_.device_id;
    if (!device.empty() &&
        ("," + chain + ",").find("," + device + ",") != std::string::npos) {
      return;
    }
    if (!device.empty()) {
      chain = chain.empty() ? device : chain + "," + device;
    }

    if (options_.clipboard) options_.clipboard->set_text(text);
    for (const session_info& peer : registry_->snapshot()) {
      if (peer.id == session->id || !peer.authenticated || !peer.transport) {
        continue;
      }
      uvcpp_json out;
      out["type"] = "clipboard-sync";
      out["text"] = text;
      out["hash"] = hash;
      out["chain"] = chain;
      send_json(peer.transport.get(), out);
    }
    return;
  }

  if (type == "display-mode") {
    if (!options_.capture) {
      send_error(transport.get(), "not_implemented", "capture is unavailable");
      return;
    }
    const auto width_it = j.find("width");
    const auto height_it = j.find("height");
    if (width_it == j.end() || height_it == j.end() ||
        !width_it->is_number() || !height_it->is_number()) {
      send_error(transport.get(), "bad_display_mode",
                 "width and height must be numbers");
      return;
    }
    const int width = static_cast<int>(width_it->get<double>());
    const int height = static_cast<int>(height_it->get<double>());
    if (width < 320 || height < 240 || width > 16384 || height > 16384) {
      send_error(transport.get(), "bad_display_mode",
                 "display mode out of range");
      return;
    }
    if (!options_.capture->supports_display_mode()) {
      send_error(transport.get(), "display_mode_unsupported",
                 "this capture path cannot change the display mode");
      return;
    }
    const uint64_t requested_ms = now_ms();
    const uint64_t previous_ms = g_last_display_mode_ms.load();
    if (previous_ms != 0 && requested_ms > previous_ms &&
        requested_ms - previous_ms < kDisplayModeCooldownMs) {
      send_error(transport.get(), "display_mode_busy",
                 "wait before changing the display mode again");
      return;
    }
    g_last_display_mode_ms.store(requested_ms);
    if (!options_.capture->set_display_mode(static_cast<uint16_t>(width),
                                            static_cast<uint16_t>(height))) {
      send_error(transport.get(), "display_mode_failed",
                 "the capture backend or display driver rejected this mode");
      return;
    }
    options_.capture->request_keyframe();
    uvcpp_json ack;
    ack["type"] = "display-mode";
    ack["width"] = width;
    ack["height"] = height;
    send_json(transport.get(), ack);
    return;
  }

  if (type == "clipboard") {
    if (!options_.clipboard || !options_.clipboard->available()) {
      send_error(transport.get(), "not_implemented",
                 "clipboard is unavailable on this host");
      return;
    }
    const std::string action = string_field(j, "action");
    if (action == "files") {
      // Current Explorer file list. Clients use the transfer channel to pull
      // the contents; this only describes what is on the clipboard.
      auto target = transport;
      auto post = options_.post;
      options_.clipboard->request_files(
          [target, post](std::vector<clipboard_file> files) {
            uvcpp_json list = uvcpp_json::array();
            for (const clipboard_file& file : files) {
              uvcpp_json item;
              item["name"] = file.name;
              item["path"] = file.path;
              item["size"] = file.size;
              list.push_back(std::move(item));
            }
            uvcpp_json out;
            out["type"] = "clipboard";
            out["action"] = "files";
            out["files"] = std::move(list);
            if (!post) {
              send_json(target.get(), out);
              return;
            }
            post([target, out] { send_json(target.get(), out); });
          });
      return;
    }
    if (action == "set") {
      std::string text_value = string_field(j, "text");
      constexpr std::size_t kMaxText = 192 * 1024;
      if (text_value.size() > kMaxText) {
        send_error(transport.get(), "clipboard_too_large",
                   "clipboard text exceeds 192 KiB");
        return;
      }
      options_.clipboard->set_text(std::move(text_value));
      return;
    }
    if (action == "get") {
      auto registry_ref = registry_;
      auto post = options_.post;
      const uint32_t session_id = session->id;
      options_.clipboard->request_text(
          [registry_ref, post, session_id](std::string clipboard_text) {
            if (!post) return;
            post([registry_ref, session_id,
                  clipboard_text = std::move(clipboard_text)] {
              const auto live = registry_ref->find(session_id);
              if (!live || !live->transport) return;
              uvcpp_json reply;
              reply["type"] = "clipboard";
              reply["text"] = clipboard_text;
              send_json(live->transport.get(), reply);
            });
          });
      return;
    }
    send_error(transport.get(), "bad_clipboard", "action must be set or get");
    return;
  }

  if (type == "request-frame") {
    // A polling client is proof that someone is watching. Ensure the capture
    // thread is awake even if the periodic registry check has not run yet;
    // otherwise the first request can be answered with empty frames only.
    if (options_.capture) options_.capture->set_active(true);
    maybe_send_cursor(transport.get(), session, options_);
    send_capture_frame(transport.get(), session, options_);
    send_tile_seq(transport.get(), session, options_);
    return;
  }

  if (type == "request-tiles") {
    std::vector<refresh_rect> rects;
    const auto tiles = j.find("tiles");
    if (tiles != j.end() && tiles->is_array()) {
      for (const auto& item : *tiles) {
        if (rects.size() >= 64) break;
        double x = 0;
        double y = 0;
        double w = 0;
        double h = 0;
        if (!number_field(item, "x", x) || !number_field(item, "y", y) ||
            !number_field(item, "w", w) || !number_field(item, "h", h)) {
          continue;
        }
        rects.push_back({static_cast<int>(x), static_cast<int>(y),
                         static_cast<int>(w), static_cast<int>(h)});
      }
    }
    if (std::getenv("MOLINK_AV1_LOG") != nullptr) {
      std::fprintf(stderr, "host: request-tiles for %zu rect(s)\n", rects.size());
    }
    std::string refresh_error;
    if (!send_tile_refresh(transport.get(), session, options_, rects,
                           refresh_error)) {
      // The backend cannot hand out current pixels; fall back to a keyframe.
      if (options_.capture) options_.capture->request_keyframe();
    }
    return;
  }

  if (type == "rtc-answer") {
    if (!require_auth()) return;
#if MOLINK_HAVE_WEBRTC
    if (session->rtc) {
      std::string rtc_error;
      if (!session->rtc->accept_answer(string_field(j, "sdp"), &rtc_error) &&
          !rtc_error.empty()) {
        std::fprintf(stderr, "webrtc: answer rejected: %s\n",
                     rtc_error.c_str());
      }
    }
#endif
    return;
  }

  if (type == "rtc-ice") {
    if (!require_auth()) return;
#if MOLINK_HAVE_WEBRTC
    if (session->rtc) {
      session->rtc->add_remote_candidate(string_field(j, "candidate"),
                                         string_field(j, "mid"));
    }
#endif
    return;
  }

  if (type == "stream-codec") {
    if (!require_auth()) return;
    const std::string value = string_field(j, "value");
    // Only codecs this host can actually encode are accepted, plus "auto".
    bool ok = value == "auto";
    if (!ok) {
      for (const video_encoder_backend& backend : video_encoder_backends()) {
        if (!backend.available) continue;
        if ((value == "av1" && backend.av1_stream) ||
            (value == "h264" && backend.h264_stream) ||
            (value == "vp9" && backend.vp9_stream) ||
            (value == "vp8" && backend.vp8_stream)) {
          ok = true;
          break;
        }
      }
    }
    if (!ok) {
      send_error(transport.get(), "bad_codec",
                 "this host cannot encode that codec");
      return;
    }
    if (options_.auth) options_.auth->set_stream_codec(value);
    // A codec switch has to be visible at once: start the stream for a short
    // window instead of waiting for the movement rule (or the 10 s exit dwell)
    // to notice. The encoder is re-initialised on the new codec and the first
    // frame is a keyframe.
    if (options_.capture) options_.capture->request_stream(15000);
    // The preference is host-global: every tab has to show the same value or
    // the last writer wins forever.
    uvcpp_json shared;
    shared["type"] = "stream-codec";
    shared["value"] = value;
    shared["scope"] = "global";
    for (const session_info& peer : registry_->snapshot()) {
      if (!peer.authenticated || !peer.transport) continue;
      send_json(peer.transport.get(), shared);
    }
    return;
  }

  if (type == "request-keyframe") {
    // MOLINK_STREAM_LOG=1: a viewer whose decoder works asks for a keyframe a
    // few times per session; one that cannot decode asks every throttle tick
    // (300 ms), which is a very different rate.
    if (std::getenv("MOLINK_STREAM_LOG") != nullptr) {
      std::fprintf(stderr, "host: request-keyframe\n");
    }
    if (options_.capture) options_.capture->request_keyframe();
    return;
  }

  // A pushed viewer switches the clock off when it stops being the one
  // looking at the picture -- a background tab, or a window that lost focus.
  // It then falls back to its own (much slower) pull until it turns the clock
  // back on. Without this a hidden tab keeps receiving full-rate frames.
  if (type == "video-push") {
    if (!require_auth()) return;
    // Frames the viewer skipped while it was pulling are repaired by the
    // sequence map and tile clock like any other gap, so turning the clock
    // back on does not need a keyframe.
    session->video_push = bool_field(j, "on", false);
    return;
  }

  if (type == "call") {
    if (!require_auth()) return;
    if (!options_.call) {
      send_error(transport.get(), "call_unavailable",
                 "voice calls are unavailable on this host");
      return;
    }
    const std::string action = string_field(j, "action");
    if (action == "invite") {
      std::string call_error;
      if (!options_.call->invite(session->id, call_error)) {
        send_error(transport.get(), "call_busy", call_error);
      }
    } else if (action == "hangup") {
      options_.call->hangup(session->id);
    }
    return;
  }

  if (type == "audio") {
    if (!require_auth()) return;
    const std::string action = string_field(j, "action");
    if (!options_.audio || !options_.audio->available()) {
      send_error(transport.get(), "audio_unavailable",
                 "audio capture is unavailable on this host");
      return;
    }
    if (action == "start") {
      if (audio_subscription_ != 0) return;
      std::weak_ptr<session_controller> weak = weak_from_this();
      auto post = options_.post;
      std::string audio_error;
      audio_subscription_ = options_.audio->subscribe(
          [weak, post](std::vector<uint8_t> pcm) {
            auto deliver = [weak, pcm = std::move(pcm)]() mutable {
              if (auto self = weak.lock()) self->send_audio(std::move(pcm));
            };
            if (post) {
              post(std::move(deliver));
            } else {
              deliver();
            }
          },
          audio_error);
      if (audio_subscription_ == 0) {
        send_error(transport.get(), "audio_failed", audio_error);
        return;
      }
      const audio_format format = options_.audio->format();
      uvcpp_json reply;
      reply["type"] = "audio";
      reply["action"] = "started";
      reply["sample_rate"] = format.sample_rate;
      reply["channels"] = format.channels;
      reply["bits"] = format.bits;
      send_json(transport.get(), reply);
    } else if (action == "stop") {
      if (audio_subscription_ != 0 && options_.audio) {
        options_.audio->unsubscribe(audio_subscription_);
        audio_subscription_ = 0;
      }
      uvcpp_json reply;
      reply["type"] = "audio";
      reply["action"] = "stopped";
      send_json(transport.get(), reply);
    }
    return;
  }

  if (type == "chat") {
    if (!require_auth()) return;
    const std::string action = string_field(j, "action");
    if (action == "history") {
      chat_history();
    } else if (action == "send") {
      const std::string text = string_field(j, "text");
      if (text.empty() || text.size() > 4096) {
        send_error(transport.get(), "bad_chat",
                   "chat text must be 1..4096 bytes");
        return;
      }
      if (options_.chat) {
        const std::string name =
            session->client_name.empty() ? peer_ip : session->client_name;
        options_.chat->post("remote", text, name, session->client_id,
                            session->id);
      }
    }
    return;
  }

  if (type == "file") {
    if (!require_auth()) return;
    if (!files_) return;
    const std::string action = string_field(j, "action");
    uint32_t id = 0;
    if (const auto it = j.find("id");
        it != j.end() && it->is_number()) {
      id = static_cast<uint32_t>(it->get<uint64_t>());
    }
    if (action == "offer") {
      uint64_t size = 0;
      if (const auto it = j.find("size");
          it != j.end() && it->is_number()) {
        size = it->get<uint64_t>();
      }
      files_->offer(string_field(j, "name"), size,
                    string_field(j, "dest"));
    } else if (action == "complete") {
      files_->complete(id);
    } else if (action == "cancel") {
      files_->cancel(id);
    } else if (action == "browse") {
      files_->browse(string_field(j, "path"));
    } else if (action == "request") {
      files_->request(string_field(j, "path"));
    } else if (action == "pull") {
      files_->pull(id);
    } else if (action == "clipboard-set") {
      // RDP-style paste: put the referenced remote files on the host's
      // clipboard so Explorer can paste them.
      std::vector<std::string> requested;
      if (const auto it = j.find("paths");
          it != j.end() && it->is_array()) {
        for (const auto& item : *it) {
          if (item.is_string()) requested.push_back(item.get<std::string>());
        }
      }
      const std::vector<std::string> resolved = files_->resolve_files(requested);
      const bool applied =
          options_.clipboard && options_.clipboard->available() &&
          !resolved.empty() && options_.clipboard->set_files(resolved);
      uvcpp_json out;
      out["type"] = "file";
      out["action"] = "clipboard";
      out["ok"] = applied;
      out["count"] = applied ? static_cast<int>(resolved.size()) : 0;
      if (!applied) {
        out["message"] =
            options_.clipboard && options_.clipboard->available()
                ? "no valid files to copy"
                : "the host clipboard does not support file lists";
      }
      send_json(transport.get(), out);
    } else {
      send_error(transport.get(), "bad_file_action",
                 "unknown file action: " + action);
    }
    return;
  }

  if (type == "bye") {
    transport->close(transport_close::normal, "client said bye");
    return;
  }

  if (type == "input") {
    if (!options_.input || !options_.input->available()) {
      uvcpp_json e;
      e["type"] = "error";
      e["code"] = "not_implemented";
      e["what"] = "input";
      e["message"] = "input injection is unavailable on this host";
      send_json(transport.get(), e);
      return;
    }

    std::string input_error;
    bool rejected = false;
    bool failed = false;
    auto apply = [&](const uvcpp_json& event) {
      const input_injector::status status =
          inject_one(*options_.input, event, input_error);
      if (status == input_injector::status::rejected) {
        rejected = true;
        return false;
      }
      if (status == input_injector::status::unavailable) failed = true;
      return true;
    };
    const auto events = j.find("events");
    if (events != j.end() && events->is_array()) {
      if (events->size() > 64) {
        send_error(transport.get(), "bad_input",
                   "at most 64 events per message");
        return;
      }
      for (const auto& event : *events) {
        if (!apply(event)) break;
      }
    } else {
      apply(j);
    }
    if (rejected) {
      send_error(transport.get(), "bad_input", input_error);
    } else if (failed) {
      // SendInput fails silently on the secure desktop and for elevated
      // windows; tell the client instead of dropping every event without a
      // word. Rate-limit it so a moving pointer cannot flood the socket.
      const std::uint64_t now = now_ms();
      if (now - last_input_error_ms_ >= 5000) {
        last_input_error_ms_ = now;
        send_error(transport.get(), "input_unavailable",
                   "the host could not inject input");
      }
    }
    return;
  }

  send_error(transport.get(), "unknown_type",
             "unsupported control type: " + type);
}

void session_controller::on_binary(const uint8_t* data, std::size_t len) {
  std::shared_ptr<session_transport> transport = transport_.lock();
  std::shared_ptr<session_info> session = session_.lock();
  if (!transport || !session) return;

  session->last_seen_ms = now_ms();
  if (options_.lock) {
    options_.lock->touch(!is_loopback_peer(transport->peer_ip()));
  }
  protocol::packet pkt;
  std::string err;
  if (!protocol::decode_packet(data, len, pkt, err)) {
    send_error(transport.get(), "bad_packet", err);
    return;
  }
  switch (static_cast<protocol::packet_type>(pkt.type)) {
    case protocol::packet_type::ping: {
      protocol::packet pong;
      pong.type = static_cast<uint8_t>(protocol::packet_type::pong);
      pong.session = pkt.session;
      pong.seq = pkt.seq;
      pong.ts_us = epoch_us();
      std::vector<uint8_t> out;
      if (protocol::encode_packet(pong, out, err)) {
        transport->send_binary(transport_channel::control, out.data(),
                               out.size());
      }
      break;
    }
    case protocol::packet_type::voice: {
      if (!session->authenticated || !voice_active_ || !options_.voice) break;
      options_.voice->play(pkt.payload.data(), pkt.payload.size());
      break;
    }
    case protocol::packet_type::file_data: {
      if (!session->authenticated || !files_) {
        send_error(transport.get(), "unauthorized",
                   "send a hello message with a valid token");
        break;
      }
      if (pkt.payload.size() < 16) {
        send_error(transport.get(), "bad_packet",
                   "file chunk header is truncated");
        break;
      }
      uint32_t id = 0;
      uint64_t offset = 0;
      std::memcpy(&id, pkt.payload.data(), sizeof(id));
      std::memcpy(&offset, pkt.payload.data() + 8, sizeof(offset));
      files_->chunk(id, offset, pkt.payload.data() + 16,
                    pkt.payload.size() - 16);
      break;
    }
    case protocol::packet_type::input:
      send_error(transport.get(), "not_implemented",
                 "input injection is not implemented");
      break;
    default:
      send_error(transport.get(), "unexpected_packet",
                 "packet type not accepted here");
      break;
  }
}

void session_controller::on_close(int code, const std::string& reason) {
  std::shared_ptr<session_info> session = session_.lock();
  if (!session) return;
  if (chat_subscription_ != 0 && options_.chat) {
    options_.chat->unsubscribe(chat_subscription_);
    chat_subscription_ = 0;
  }
  if (audio_subscription_ != 0 && options_.audio) {
    options_.audio->unsubscribe(audio_subscription_);
    audio_subscription_ = 0;
  }
  if (call_subscription_ != 0 && options_.call) {
    options_.call->unsubscribe(call_subscription_);
    call_subscription_ = 0;
  }
  if (options_.call) options_.call->hangup(session->id);
  stop_voice();
  if (files_) files_->close_all();
  if (session->access_request_id != 0 && options_.access_requests) {
    options_.access_requests->remove(session->access_request_id);
  }
  registry_->remove(session->id);
  // Stop grabbing the screen once nobody is watching.
  if (options_.capture && !registry_->has_authenticated()) {
    options_.capture->set_active(false);
  }
  // A dropped connection must not leave a button or modifier held down.
  if (options_.input && !registry_->has_authenticated()) {
    options_.input->release_all();
  }
  (void)code;
  (void)reason;
}

uvcpp::uvcpp_web_ws_handler make_ws_handler(
    std::shared_ptr<session_registry> registry, session_options options) {
  return [registry, options](uvcpp::uvcpp_web_ws_request& ws) {
    uvcpp::uvcpp_ws_connection* conn = ws.connection();
    if (conn == nullptr) return;

    // Control messages are small JSON documents; the bulk data travels in the
    // other direction as binary packets.
    conn->set_max_message_size(1024u * 1024u);

    auto transport = std::make_shared<ws_transport>(
        conn, ws.request().peer_ip(), ws.request().peer_port());
    auto controller =
        std::make_shared<session_controller>(registry, transport, options);
    const std::string* token = ws.query("token");
    controller->start(token);
  };
}

session_supervisor::session_supervisor(
    uvcpp::uvcpp_web_app& app, std::shared_ptr<session_registry> registry,
    session_options options)
    : app_(app), registry_(std::move(registry)), options_(std::move(options)) {}

session_supervisor::~session_supervisor() { stop(); }

void session_supervisor::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { run(); });
  if (options_.post && options_.capture) {
    push_thread_ = std::thread([this] { run_push_clock(); });
  }
}

void session_supervisor::stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
  if (push_thread_.joinable()) push_thread_.join();
}

/// The host side of the frame clock (Phase 2 of the media transport rebuild).
///
/// Every tick offers each push session the newest capture; frames the viewer
/// never got to are never sent, so a slow link does not build a backlog of
/// obsolete deltas. The rate follows the capture target and is halved while a
/// viewer reports that it cannot paint what it already receives -- the same
/// feedback the pull path uses to lower detail, applied to cadence instead.
void session_supervisor::run_push_clock() {
  video_push_clock clock(options_.capture ? options_.capture->target_fps()
                                          : video_push_clock::kDefaultFps);
  while (running_.load()) {
    std::this_thread::sleep_for(
        std::chrono::milliseconds(clock.interval_ms()));
    if (!running_.load()) break;
    if (!registry_->has_authenticated() || !options_.capture) continue;
    clock.set_target_fps(options_.capture->target_fps());
    const uint64_t now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    if (!clock.due(now_ms)) continue;

    // Read the viewer feedback here rather than inside the posted lambda: the
    // supervisor must not capture `this`, a post can outlive stop().
    double slowest = 0.0;
    bool dropping = false;
    bool any = false;
    for (const session_info& peer : registry_->snapshot()) {
      if (!peer.authenticated || !peer.video_push || !peer.transport) continue;
      any = true;
      if (peer.viewer_stats_ms == 0 || now_ms - peer.viewer_stats_ms > 3000) {
        continue;
      }
      const double fps = peer.viewer_paint_fps;
      if (fps <= 0.0) continue;
      slowest = slowest <= 0.0 ? fps : std::min(slowest, fps);
      if (peer.viewer_dropped > 0) dropping = true;
    }
    if (!any) continue;
    clock.note_viewer(slowest, dropping ? 1 : 0, now_ms);

    auto registry_ref = registry_;
    auto post = options_.post;
    session_options options = options_;
    post([registry_ref, options] {
      for (const session_info& peer : registry_ref->snapshot()) {
        if (!peer.authenticated || !peer.video_push || !peer.transport) {
          continue;
        }
        const std::shared_ptr<session_info> live = registry_ref->find(peer.id);
        if (!live || !live->transport) continue;
        session_transport* transport = live->transport.get();
        send_capture_frame(transport, live, options, /*push=*/true);
        // These rode along with `request-frame` in the pull model. A pushed
        // session sends no requests, so the clock has to carry them or the
        // cursor freezes and the sequence map that names stale cells never
        // reaches the viewer.
        maybe_send_cursor(transport, live, options);
        send_tile_seq(transport, live, options);
      }
    });
  }
}

void session_supervisor::run() {
  using namespace std::chrono;
  const auto tick = milliseconds(250);
  int last_display = -1;
  while (running_.load()) {
    for (int i = 0; i < 4 && running_.load(); ++i) {
      std::this_thread::sleep_for(tick);
    }
    if (!running_.load()) break;

    // AV1 patches are only safe when every watching session can decode them:
    // a tile-only viewer would miss the rectangles removed from the base
    // frame. Re-evaluate once a second as sessions come and go.
    if (options_.capture) {
      bool live = false;
      bool all_av1 = true;
      bool all_stream = true;
      bool all_h264_stream = true;
      bool all_vp9_stream = true;
      bool all_vp8_stream = true;
      int viewers = 0;
      for (const session_info& peer : registry_->snapshot()) {
        if (!peer.authenticated) continue;
        live = true;
        ++viewers;
        if (!peer.av1) all_av1 = false;
        if (!peer.av1_stream) all_stream = false;
        if (!peer.h264_stream) all_h264_stream = false;
        if (!peer.vp9_stream) all_vp9_stream = false;
        if (!peer.vp8_stream) all_vp8_stream = false;
      }
      // The host settings page can switch AV1 off entirely; it still only
      // runs when every watching client can decode it.
      const bool enabled =
          !options_.auth || options_.auth->settings().av1_enabled;
      const bool allow = enabled && live && all_av1;
      if (std::getenv("MOLINK_AV1_LOG") != nullptr && allow != av1_allowed_) {
        std::fprintf(stderr, "host: av1 allowed=%d\n", allow ? 1 : 0);
        av1_allowed_ = allow;
      }
      // Single-frame AV1 (per-rectangle patches and full-screen intra) is
      // retired: on this hardware it re-initialises the encoder per rectangle
      // and falls back to tiles under movement, which is exactly the "AV1 is
      // slower and larger" behaviour that was reported. AV1 now means the
      // low-delay stream only; the stream encoder below is the sole AV1 path.
      options_.capture->set_av1_allowed(false);
      // The low-delay stream needs a stateful decoder on every viewer; a
      // single session without one keeps the host on tiles and patches.
      // AV1 is preferred; when it is not decodable by everyone but H.264 is,
      // the stream switches codec instead of dropping back to tiles.
      // A pinned codec wins when every viewer can decode it; the automatic
      // hardware-first order applies otherwise. The capture falls back to
      // tiles when this host cannot actually encode the pinned one.
      const std::string preference =
          options_.auth ? options_.auth->settings().stream_codec : "auto";
      stream_choice pinned = stream_choice::none;
      if (preference == "av1" && allow && all_stream) {
        pinned = stream_choice::av1;
      } else if (preference == "h264" && all_h264_stream) {
        pinned = stream_choice::h264;
      } else if (preference == "vp9" && all_vp9_stream) {
        pinned = stream_choice::vp9;
      } else if (preference == "vp8" && all_vp8_stream) {
        pinned = stream_choice::vp8;
      }
      const std::string video_mode =
          options_.auth ? options_.auth->settings().video_mode : "mixed";
      if (options_.capture) {
        options_.capture->set_stream_always(video_mode == "stream");
      }
      const bool stream_allowed = video_mode != "tiles";
      const stream_choice stream =
          live && viewers > 0 && stream_allowed
              ? (pinned != stream_choice::none
                     ? pinned
                     : choose_video_codec(allow, all_stream, all_h264_stream,
                                          all_vp9_stream, all_vp8_stream))
              : stream_choice::none;
      options_.capture->set_av1_stream_allowed(stream == stream_choice::av1);
      options_.capture->set_h264_stream_allowed(stream == stream_choice::h264);
      if (std::getenv("MOLINK_STREAM_LOG") != nullptr) {
        static int last_stream = -1;
        if (static_cast<int>(stream) != last_stream) {
          last_stream = static_cast<int>(stream);
          std::fprintf(stderr,
                       "host: stream choice=%d (all av1=%d h264=%d vp9=%d vp8=%d)\n",
                       static_cast<int>(stream), all_stream ? 1 : 0,
                       all_h264_stream ? 1 : 0, all_vp9_stream ? 1 : 0,
                       all_vp8_stream ? 1 : 0);
        }
      }
      options_.capture->set_vp9_stream_allowed(stream == stream_choice::vp9);
      options_.capture->set_vp8_stream_allowed(stream == stream_choice::vp8);
      if (!live) options_.capture->set_active(false);
    }

    // Back-pressure from the viewers. A client that keeps dropping frames, or
    // paints far below the rate it asked for, is the limit: no amount of host
    // CPU makes its pipeline faster, so the capture sends less detail instead
    // of letting the client's own controller collapse to the minimum rate.
    if (options_.capture) {
      const uint64_t now_ms = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count());
      const int target = options_.capture->target_fps();
      bool behind = false;
      double slowest_fps = 0.0;
      for (const session_info& peer : registry_->snapshot()) {
        if (!peer.authenticated || !peer.has_image) continue;
        if (peer.viewer_stats_ms == 0 ||
            now_ms - peer.viewer_stats_ms > 3000) {
          continue;
        }
        if (peer.viewer_dropped >= 3) behind = true;
        if (target > 0 && peer.viewer_paint_fps > 0) {
          if (peer.viewer_paint_fps < target * 0.6) behind = true;
          slowest_fps = slowest_fps <= 0.0
                            ? peer.viewer_paint_fps
                            : (std::min)(slowest_fps, peer.viewer_paint_fps);
        }
      }
      if (behind) {
        ++viewer_behind_streak_;
      } else {
        viewer_behind_streak_ = 0;
      }
      options_.capture->set_viewer_pressure(viewer_behind_streak_ >= 2);
      // The quality ladder moves on the same feedback, but only while the
      // host actually produced frames: a static screen sends almost nothing,
      // and "no packets because nothing changed" is not a stall.
      const capture_stats rate_stats = options_.capture->stats();
      const bool demand = last_capture_at_.time_since_epoch().count() != 0 &&
                          rate_stats.frames > last_capture_frames_ + 2;
      options_.capture->note_viewer_rate(slowest_fps, target, demand, now_ms);
    }

    // Once a second, report the rate the capture thread achieved and how long
    // it spends building/encoding one frame. A client cannot tell a slow host
    // from a static screen on its own; together with latency and dropped
    // frames this lets its frame-rate controller lower the requested rate when
    // the encoder (CPU) is the bottleneck.
    if (options_.capture && options_.post && registry_->has_authenticated()) {
      const capture_stats stats = options_.capture->stats();
      const auto now = std::chrono::steady_clock::now();
      if (last_capture_at_.time_since_epoch().count() == 0) {
        last_capture_at_ = now;
        last_capture_frames_ = stats.frames;
      } else {
        const double seconds =
            std::chrono::duration<double>(now - last_capture_at_).count();
        double fps = 0.0;
        if (seconds > 0.0 && stats.frames >= last_capture_frames_) {
          fps = static_cast<double>(stats.frames - last_capture_frames_) /
                seconds;
        }
        last_capture_at_ = now;
        last_capture_frames_ = stats.frames;
        char text[160] = {};
        std::snprintf(text, sizeof(text),
                      "{\"type\":\"capture-stats\",\"fps\":%.1f,"
                      "\"target_fps\":%d,\"build_ms\":%.1f}",
                      fps, options_.capture->target_fps(),
                      static_cast<double>(options_.capture->build_time_us()) /
                          1000.0);
        auto registry_ref = registry_;
        auto post = options_.post;
        const std::string message(text);
        post([registry_ref, message] {
          for (const session_info& peer : registry_ref->snapshot()) {
            if (!peer.authenticated || !peer.transport) continue;
            peer.transport->send_text(transport_channel::control, message);
          }
        });
      }
    }

    // Once a second, tell every session with a picture which 128 px cells
    // changed and when. A client compares this with the frames it applied and
    // asks for the cells it missed, so one lost refresh cannot leave a stale
    // rectangle on screen forever.
    if (options_.capture && options_.post && registry_->has_authenticated()) {
      std::vector<uint8_t> clock;
      if (options_.capture->tile_clock(clock)) {
        auto registry_ref = registry_;
        auto post = options_.post;
        const uint64_t now = epoch_us();
        post([registry_ref, clock = std::move(clock), now] {
          for (const session_info& peer : registry_ref->snapshot()) {
            if (!peer.authenticated || !peer.transport || !peer.has_image) {
              continue;
            }
            std::vector<uint8_t> bytes;
            std::string error;
            if (!protocol::encode_packet_into(
                    bytes,
                    static_cast<uint8_t>(protocol::packet_type::tile_clock), 0,
                    peer.id, 0, now, clock.data(), clock.size(), error)) {
              continue;
            }
            peer.transport->send_binary(transport_channel::video, bytes.data(),
                                        bytes.size());
          }
        });
      }
    }

    // Push local (host) clipboard changes to every watching session. Remote
    // updates already marked their hash before set_text, so they are skipped.
    if (options_.clipboard && options_.clipboard->available() &&
        registry_->has_authenticated() &&
        !g_clipboard_sync.fetch_pending.exchange(true)) {
      auto registry_ref = registry_;
      auto post = options_.post;
      const std::string device = options_.device_id;
      options_.clipboard->request_text(
          [registry_ref, post, device](std::string text) {
            g_clipboard_sync.fetch_pending.store(false);
            if (text.empty() || text.size() > 192 * 1024) return;
            const std::string hash = clipboard_hash(text);
            if (!clipboard_take_hash(hash)) return;
            if (!post) return;
            post([registry_ref, text, hash, device] {
              for (const session_info& peer : registry_ref->snapshot()) {
                if (!peer.authenticated || !peer.transport) continue;
                uvcpp_json out;
                out["type"] = "clipboard-sync";
                out["text"] = text;
                out["hash"] = hash;
                out["chain"] = device;
                send_json(peer.transport.get(), out);
              }
            });
          });
    }

    // Explorer file lists use CF_HDROP, not text. Describe them whenever the
    // list changes; a client downloads the contents over the file channel.
    if (options_.clipboard && options_.clipboard->available() &&
        registry_->has_authenticated() &&
        !g_clipboard_sync.files_pending.exchange(true)) {
      auto registry_ref = registry_;
      auto post = options_.post;
      options_.clipboard->request_files(
          [registry_ref, post](std::vector<clipboard_file> files) {
            g_clipboard_sync.files_pending.store(false);
            if (files.empty()) return;
            std::string key;
            for (const clipboard_file& file : files) {
              key += file.path;
              key += '\n';
            }
            if (!clipboard_take_hash("files:" + clipboard_hash(key))) return;
            if (!post) return;
            post([registry_ref, files] {
              uvcpp_json list = uvcpp_json::array();
              for (const clipboard_file& file : files) {
                uvcpp_json item;
                item["name"] = file.name;
                item["path"] = file.path;
                item["size"] = file.size;
                list.push_back(std::move(item));
              }
              for (const session_info& peer : registry_ref->snapshot()) {
                if (!peer.authenticated || !peer.transport) continue;
                uvcpp_json out;
                out["type"] = "clipboard";
                out["action"] = "files";
                out["files"] = list;
                send_json(peer.transport.get(), out);
              }
            });
          });
    }

    const uint64_t now = now_ms();
    if (options_.lock &&
        options_.lock->should_lock(now, registry_->has_authenticated())) {
      options_.lock->lock();
      std::fprintf(stderr, "molink: UI locked after %d seconds idle\n",
                   options_.lock->timeout_seconds());
    }
    if (options_.capture) {
      // Safety net in case a close callback was missed during shutdown.
      options_.capture->set_active(registry_->has_authenticated());
    }
    const std::vector<session_info> sessions = registry_->snapshot();
    if (options_.capture) {
      // The capture target is host-wide; tell every other session when one of
      // them switches monitors so their selector does not go stale.
      const int display = options_.capture->current_display();
      if (last_display < 0) {
        last_display = display;
      } else if (display != last_display) {
        last_display = display;
        for (const session_info& s : sessions) {
          if (!s.authenticated) continue;
          const uint32_t id = s.id;
          app_.post([registry = registry_, id, display] {
            const auto live = registry->find(id);
            if (!live || !live->transport) return;
            uvcpp_json message;
            message["type"] = "display";
            message["index"] = display;
            send_json(live->transport.get(), message);
          });
        }
      }
    }
    for (const session_info& s : sessions) {
      if (!s.authenticated) {
        // A pending approval is a deliberate wait, not a stuck client.
        const uint64_t limit = s.awaiting_approval
                                   ? 120000
                                   : static_cast<uint64_t>(
                                         options_.hello_timeout_ms);
        if (now - s.connected_ms > limit) {
          const uint32_t id = s.id;
          app_.post([registry = registry_, id] {
            const auto live = registry->find(id);
            if (!live) return;
            // Remove before closing so a recycled transport cannot be closed
            // again by the next supervisor tick.
            registry->remove(id);
            if (live->transport) {
              live->transport->close(transport_close::policy,
                                     "hello timeout");
            }
          });
        }
        continue;
      }

      if (now - s.last_ping_ms >=
          static_cast<uint64_t>(options_.heartbeat_ms)) {
        const uint32_t id = s.id;
        app_.post([registry = registry_, id] {
          const auto live = registry->find(id);
          if (!live || !live->transport || !live->authenticated) return;
          uvcpp_json j;
          j["type"] = "ping";
          j["t"] = now_ms();
          send_json(live->transport.get(), j);
          live->last_ping_ms = now_ms();
        });
      }

      if (!s.awaiting_approval &&
          now - s.last_seen_ms >
              static_cast<uint64_t>(options_.idle_close_ms)) {
        const uint32_t id = s.id;
        app_.post([registry = registry_, id] {
          const auto live = registry->find(id);
          if (!live) return;
          // Drop the record first: libuvcpp may recycle the connection object
          // for a later handshake, and a stale close would hit the new peer.
          registry->remove(id);
          if (live->transport) {
            live->transport->close(transport_close::going_away,
                                   "idle timeout");
          }
        });
      }
    }
  }
}

}  // namespace molink
