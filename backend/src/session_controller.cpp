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
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <handle/uvcpp_timer.h>
#include <web/uvcpp_ws_connection.h>
#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_json.h>

#include "config.h"
#include "capture/frame_encode.h"
#include "diag_log.h"
#include "compression.h"
#include "capture/replay_policy.h"
#include "capture/video_push.h"
#include "clipboard_pull.h"
#include "input/input_event.h"
#if MOLINK_HAVE_WEBRTC
#include "webrtc/rtc_session.h"
#endif
#include "protocol/frame_packet.h"
#include "service_control.h"
#include "session_preset.h"
#include "transport/switchable_transport.h"
#include "transport/ws_transport.h"
#include "util.h"
#include "version.h"
#include "video_mode.h"

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
  /// File-list signatures (path per line) announced or written recently.
  /// Suppressing a signature for two minutes is what stops a selection this
  /// machine just put on the clipboard from travelling back to the viewer that
  /// supplied it (`file (1)(2)` ping-pong).
  std::unordered_map<std::string, uint64_t> files_recent;
  /// The list most recently announced (or written for a viewer). Unlike the
  /// two-minute echo window this is not time bounded: the periodic poll must
  /// not re-announce an unchanged clipboard every two minutes, which would
  /// make a viewer download its own files back.
  std::string last_files_signature;
  bool has_last_files = false;
};

clipboard_sync_state g_clipboard_sync;

constexpr uint64_t kClipboardFileEchoMs = 2ull * 60 * 1000;
constexpr std::size_t kClipboardFileEchoMax = 64;
/// One clipboard-pull range is at most this many bytes. The viewer uses the
/// same size, so a normal download is one packet per request.
constexpr uint64_t kClipboardPullChunkBytes = 256ull * 1024;
// The id list is capped by count only: each entry is metadata, so a large
// file costs a few bytes here. The client decides whether it can stream it
// (virtual clipboard) or download it (its own, smaller, download caps).
constexpr std::size_t kClipboardPullMaxFiles = 8;
constexpr uint64_t kClipboardPullMaxFileBytes = 1ull * 1024 * 1024 * 1024 * 1024;
constexpr uint64_t kClipboardPullMaxTotalBytes =
    4ull * 1024 * 1024 * 1024 * 1024;

std::string clipboard_files_signature(
    const std::vector<clipboard_file>& files) {
  std::string signature;
  for (const clipboard_file& file : files) {
    // Keep the historical path-only key for real files: clipboard_mark_files_written
    // records just the paths, and changing the key would defeat echo
    // suppression. A virtual entry has no path, so name/size/generation are
    // what makes two data-object offers distinguishable.
    if (!file.path.empty()) {
      signature += file.path;
    } else {
      signature += "@stream:";
      signature += file.name;
      signature += ':';
      signature += std::to_string(file.size);
      signature += ':';
      signature += std::to_string(file.stream_id);
    }
    signature += '\n';
  }
  return signature;
}

/// False when this exact file list was published (or written by a viewer)
/// within the echo window. Marks a new list as seen.
bool clipboard_take_files_signature(const std::string& signature) {
  if (signature.empty()) return false;
  const uint64_t now = now_ms();
  std::lock_guard<std::mutex> lock(g_clipboard_sync.mutex);
  if (g_clipboard_sync.has_last_files &&
      g_clipboard_sync.last_files_signature == signature) {
    return false;
  }
  g_clipboard_sync.last_files_signature = signature;
  g_clipboard_sync.has_last_files = true;
  auto& recent = g_clipboard_sync.files_recent;
  const auto it = recent.find(signature);
  if (it != recent.end() && now >= it->second &&
      now - it->second < kClipboardFileEchoMs) {
    return false;
  }
  // Bound the table: drop expired entries when it grows.
  if (recent.size() >= kClipboardFileEchoMax) {
    for (auto entry = recent.begin(); entry != recent.end();) {
      if (now < entry->second || now - entry->second >= kClipboardFileEchoMs) {
        entry = recent.erase(entry);
      } else {
        ++entry;
      }
    }
  }
  recent[signature] = now;
  return true;
}

/// The clipboard now holds text (or nothing). A later copy of the same files
/// is a new action and must be announced again.
void clipboard_reset_files_signature() {
  std::lock_guard<std::mutex> lock(g_clipboard_sync.mutex);
  g_clipboard_sync.last_files_signature.clear();
  g_clipboard_sync.has_last_files = false;
}

/// Remembers files the host just materialised for a viewer so the clipboard
/// watcher does not publish them straight back.
void clipboard_mark_files_written(const std::vector<std::string>& paths) {
  std::vector<clipboard_file> files;
  files.reserve(paths.size());
  for (const std::string& path : paths) {
    if (path.empty()) continue;
    clipboard_file file;
    file.path = path;
    files.push_back(std::move(file));
  }
  const std::string signature = clipboard_files_signature(files);
  if (!signature.empty()) clipboard_take_files_signature(signature);
}

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
  // Every error handed to a viewer is also a host-side diagnostic. `err_code`
  // rather than `code` keeps the redactor from mistaking a machine error name
  // for a verification code.
  std::vector<diag_field> fields;
  fields.push_back({"err_code", code});
  if (transport != nullptr) {
    fields.push_back({"peer", transport->peer_ip()});
    fields.push_back({"proto", transport->name()});
  }
  diag().log(diag_level::error, "session.error", message, fields);
  uvcpp_json j;
  j["type"] = "error";
  j["code"] = code;
  j["message"] = message;
  send_json(transport, j);
}

/// Tells the viewer that a frame was dropped by the host's local send queue.
/// This is deliberately not an `error`: the path is healthy and the host is
/// already rebuilding the reference chain, so the client must not report a
/// session error or fall back from P2P on it.
void send_stream_gap(session_transport* transport) {
  uvcpp_json j;
  j["type"] = "stream-gap";
  j["reason"] = "backpressure";
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

/// `MOLINK_CLIPBOARD_STREAM=0` is the kill switch for the virtual-file path:
/// even a viewer that advertises the capability falls back to the eager
/// upload, without needing another build.
bool clipboard_stream_capable(const uvcpp_json& j) {
  const char* env = std::getenv("MOLINK_CLIPBOARD_STREAM");
  if (env != nullptr && std::string(env) == "0") return false;
  return bool_field(j, "clipboard_stream", false);
}

/// Publish a host clipboard file list to every authenticated viewer.
///
/// One list is sent in two shapes:
///  * `clipboard-sync.files` names the files with a session-scoped 1-based id.
///    New viewers pull the bytes with `clipboard-pull` and never see a host
///    path; this is the path that works through nested RDP.
///  * the older `clipboard/files` message keeps the files panel (and older
///    clients) working, and is the fallback for a plain browser page that
///    cannot write the local clipboard.
///
/// The signature is checked first so a list this machine just wrote for a
/// viewer (or already published) does not echo back.
void send_clipboard_files_to(
    const std::shared_ptr<session_registry>& registry, const session_info& peer,
    const std::vector<clipboard_file>& files, bool nested_rdp) {
  if (!peer.authenticated || !peer.transport) return;
  {
    uint64_t total = 0;
    for (const clipboard_file& file : files) total += file.size;
    std::vector<diag_field> fields;
    fields.push_back({"peer", peer.peer_ip});
    fields.push_back({"files", std::to_string(files.size())});
    fields.push_back({"bytes", std::to_string(total)});
    fields.push_back({"nested_rdp", nested_rdp ? "1" : "0"});
    diag().log(diag_level::info, "clipboard.announce", std::string(), fields);
  }
  uvcpp_json legacy_list = uvcpp_json::array();
  for (const clipboard_file& file : files) {
    uvcpp_json legacy;
    legacy["name"] = file.name;
    legacy["path"] = file.path;
    legacy["size"] = file.size;
    legacy_list.push_back(std::move(legacy));
  }

  // The id list is capped exactly like the viewer caps it: 8 files, 256 MiB
  // each, 2 GiB total. The viewer learns how many were left out.
  std::size_t advertised = 0;
  uint64_t total = 0;
  for (const clipboard_file& file : files) {
    if (advertised >= kClipboardPullMaxFiles) break;
    if (file.size > kClipboardPullMaxFileBytes) break;
    if (total + file.size > kClipboardPullMaxTotalBytes) break;
    total += file.size;
    ++advertised;
  }
  const std::size_t omitted = files.size() - advertised;

  if (peer.clipboard_pull) {
    // Only a client that advertised clipboard_pull gets the id list. An
    // older client's `clipboard-sync` handler writes `text` unconditionally,
    // so a files-only message would clear its text clipboard.
    std::vector<clipboard_offer_entry> offers;
    offers.reserve(advertised);
    uvcpp_json pull_list = uvcpp_json::array();
    for (std::size_t i = 0; i < advertised; ++i) {
      clipboard_offer_entry offer;
      offer.id = static_cast<uint32_t>(i + 1);
      offer.name = files[i].name;
      offer.size = files[i].size;
      offers.push_back(offer);

      uvcpp_json item;
      item["id"] = static_cast<uint64_t>(offer.id);
      item["name"] = offer.name;
      item["size"] = offer.size;
      // Kept for the browser fallback (the files panel downloads by path).
      item["path"] = files[i].path;
      pull_list.push_back(std::move(item));
    }
    registry->set_clipboard_offer(peer.id, std::move(offers), nested_rdp);
    uvcpp_json out;
    out["type"] = "clipboard-sync";
    out["files"] = pull_list;
    if (nested_rdp) out["nested_rdp"] = true;
    if (omitted > 0) out["omitted"] = static_cast<uint64_t>(omitted);
    send_json(peer.transport.get(), out);
  }

  uvcpp_json legacy_out;
  legacy_out["type"] = "clipboard";
  legacy_out["action"] = "files";
  legacy_out["files"] = legacy_list;
  send_json(peer.transport.get(), legacy_out);
}

void publish_clipboard_files(
    const std::shared_ptr<session_registry>& registry,
    const std::function<void(std::function<void()>)>& post,
    std::vector<clipboard_file> files, bool nested_rdp) {
  files.erase(std::remove_if(files.begin(), files.end(),
                             [](const clipboard_file& file) {
                               return !clipboard_file_has_bytes(file) ||
                                      file.name.empty();
                             }),
              files.end());
  if (files.empty()) return;
  const std::string signature = clipboard_files_signature(files);
  if (!clipboard_take_files_signature(signature)) return;

  auto deliver = [registry, files = std::move(files), nested_rdp] {
    for (const session_info& peer : registry->snapshot()) {
      send_clipboard_files_to(registry, peer, files, nested_rdp);
    }
  };
  if (post) {
    post(std::move(deliver));
  } else {
    deliver();
  }
}

/// Decodes the JSON wire form into the platform-neutral `input_event` the
/// injector test covers. Validation errors are filled in here so `inject_one`
/// never reports an empty `bad_input`.
bool decode_input_event(const uvcpp_json& j, input_event& out,
                        std::string& err) {
  if (!j.is_object()) {
    err = "input event must be an object";
    return false;
  }
  out = input_event{};
  const std::string kind = string_field(j, "k");
  out.kind_name = kind;
  if (kind == "sas") {
    out.type = input_event::kind::sas;
    return true;
  }
  if (kind == "move") {
    out.type = input_event::kind::move;
    if (!number_field(j, "x", out.x) || !number_field(j, "y", out.y)) {
      err = "move needs normalized x and y";
      return false;
    }
    out.has_xy = true;
    return true;
  }
  if (kind == "button") {
    out.type = input_event::kind::button;
    double button = 1;
    if (!number_field(j, "button", button)) {
      err = "button needs a button number";
      return false;
    }
    out.button = static_cast<int>(button);
    out.has_button = true;
    out.down = bool_field(j, "down", true);
    return true;
  }
  if (kind == "wheel") {
    out.type = input_event::kind::wheel;
    number_field(j, "dx", out.dx);
    number_field(j, "dy", out.dy);
    return true;
  }
  if (kind == "key") {
    out.type = input_event::kind::key;
    out.code = string_field(j, "code");
    if (out.code.empty()) {
      err = "key needs a code";
      return false;
    }
    out.down = bool_field(j, "down", true);
    return true;
  }
  if (kind == "text") {
    out.type = input_event::kind::text;
    out.text = string_field(j, "text");
    if (out.text.empty() || out.text.size() > 4096) {
      err = "text must be 1..4096 bytes";
      return false;
    }
    return true;
  }
  err = "unknown input event kind: " + kind;
  return false;
}

input_injector::status inject_one(input_injector& input, const uvcpp_json& event,
                                  std::string& err) {
  input_event decoded;
  if (!decode_input_event(event, decoded, err)) {
    return input_injector::status::rejected;
  }
  return inject_input_event(input, decoded, err, &send_secure_attention);
}

bool send_frame_payload(session_transport* transport,
                        const std::shared_ptr<session_info>& session,
                        uint16_t flags, uint64_t ts_us,
                        const std::vector<uint8_t>& payload,
                        const session_options* opt = nullptr) {
  std::vector<uint8_t> bytes;
  std::string err;
  const uint32_t seq = session->out_seq++;
  if (!protocol::encode_packet_into(
          bytes, static_cast<uint8_t>(protocol::packet_type::frame), flags,
          session->id, seq, ts_us, payload.data(), payload.size(), err)) {
    send_error(transport, "frame_build_failed", err);
    return false;
  }
  const bool accepted = transport->send_binary(transport_channel::video,
                                               bytes.data(), bytes.size());
  const uint64_t now = now_ms();
  const video_send_status status = handle_video_send_result(
      accepted, transport->is_open(), now, session->video_drop,
      // A dropped frame leaves the viewer without a reference it needs. Ask
      // the capture for a new baseline, but not on every frame of a burst.
      [&session, opt, now] {
        if (!opt || !opt->capture) return;
        // Keep the capture-side throttle in sync so the caller's own recovery
        // path does not ask for the same keyframe again in this tick.
        session->last_keyframe_request_ms = now;
        opt->capture->request_keyframe();
      },
      [transport, opt] {
        if (opt) send_stream_gap(transport);
      },
      [transport] { send_error(transport, "send_failed", "could not queue frame"); });
  if (status != video_send_status::sent) return false;
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
    case static_cast<uint8_t>(protocol::codec::av1_stream):
      // The low-delay stream capability. `av1` is the retired single-frame
      // path; gating the stream on it silently dropped every AV1 frame once
      // the proxy stopped advertising the old flag.
      return session->av1_stream;
    default:
      return false;
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
                         const captured_frame& frame,
                         const session_options* opt = nullptr) {
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
        frame.av1_payload, opt);
  }
  if (!send_frame_payload(transport, session,
                          frame.keyframe ? protocol::kPacketFlagKeyframe : 0,
                          frame.ts_us, frame.payload, opt)) {
    return false;
  }
  if (session->av1 && !frame.av1_payload.empty()) {
    send_frame_payload(transport, session, protocol::kPacketFlagTileRefresh,
                       frame.ts_us, frame.av1_payload, opt);
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
  // A refresh that covers the whole screen leaves the viewer with a complete
  // picture, exactly like a keyframe. Mark it as one in the frame header: the
  // page refuses a tile refresh until it has an image, so without the flag the
  // bootstrap refresh was thrown away and the session stayed on "waiting for
  // the first frame" even though the host had just sent it the whole desktop.
  uint64_t covered = 0;
  for (const protocol::tile_blob& tile : tiles) {
    covered += static_cast<uint64_t>(tile.w) * tile.h;
  }
  const uint64_t screen =
      static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
  const bool full_refresh = screen > 0 && covered * 10 >= screen * 9;
  protocol::frame_header header;
  header.frame_seq = 0;
  header.codec = static_cast<uint8_t>(protocol::codec::bgra);
  header.flags = protocol::kFrameFlagPartial;
  if (full_refresh) header.flags |= protocol::kFrameFlagKeyframe;
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
  if (!send_frame_payload(transport, session,
                          protocol::kPacketFlagTileRefresh, epoch_us(),
                          payload, &opt)) {
    return false;
  }
  // The full-screen refresh above was flagged as a keyframe, so it counts as
  // the session's first complete picture; tile clocks can start flowing.
  if (full_refresh) session->has_image = true;
  return true;
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
  // Per-session keyframe throttle. The push clock calls this function at the
  // capture rate; a session without a picture would otherwise request a
  // keyframe every tick, and each request makes the capture publish a
  // full-screen frame -- expensive, and it marks every tile-clock cell as
  // changed, which sends every *other* viewer after the whole screen too.
  const auto ask_keyframe = [&opt, &session]() {
    if (!opt.capture) return;
    const uint64_t now = now_ms();
    if (session->last_keyframe_request_ms != 0 &&
        now - session->last_keyframe_request_ms < 1000) {
      return;
    }
    session->last_keyframe_request_ms = now;
    opt.capture->request_keyframe();
  };

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
      // No keyframe is left in the replay history. On a tile-capture host the
      // history can have evicted the only keyframe while the desktop stayed
      // still (DXGI produces no frames, and a GDI host may have been idle for
      // longer than the one-second history). Asking the capture is the plan A,
      // but a capture that is stuck or switching desktops can leave the viewer
      // on "waiting for a picture" indefinitely. A full-screen refresh reads
      // the *current* pixels directly and is not gated on the capture's
      // keyframe state, so it always bootstraps the picture. Only for a tile
      // capture: a stream viewer would have its tiles overwritten by the next
      // stream keyframe anyway.
      if (!latest->av1_only) {
        const uint64_t now = now_ms();
        if (session->last_keyframe_request_ms == 0 ||
            now - session->last_keyframe_request_ms >= 1000) {
          session->last_keyframe_request_ms = now;
          const std::vector<refresh_rect> full{{0, 0, latest->width,
                                                latest->height}};
          std::string refresh_error;
          if (send_tile_refresh(transport, session, opt, full,
                                refresh_error)) {
            session->last_capture_seq = static_cast<int64_t>(latest->seq);
            return;
          }
        }
      }
      ask_keyframe();
      if (!push) {
        send_empty_frame(transport, session, latest->width, latest->height,
                         false);
      }
      return;
    }
    // A session that joins while the capture is on the stream must not start
    // from a tile keyframe: that full-screen tile frame is the expensive one
    // and it made the first second of a session stutter before the stream
    // took over. Wait for the stream's own keyframe instead, which follows
    // within a frame.
    // Only while the newest frame really is a stream frame: a capture whose
    // encoder fell back to tiles (or whose viewer set cannot decode the
    // stream) must let the session start from the tile keyframe, otherwise
    // it waits for a keyframe that will never be produced.
    if (opt.capture->stream_on() && latest->av1_only) {
      const std::shared_ptr<const captured_frame> start =
          opt.capture->find(static_cast<uint32_t>(start_seq));
      if (!start || !start->av1_only) {
        ask_keyframe();
        if (!push) {
          send_empty_frame(transport, session, latest->width, latest->height,
                           false);
        }
        return;
      }
    }
    session->last_capture_seq = start_seq - 1;
  }

  const int64_t last = session->last_capture_seq;
  if (std::getenv("MOLINK_STREAM_LOG") != nullptr) {
    static thread_local uint64_t last_idle_log = 0;
    const uint64_t idle_now = now_ms();
    if (idle_now - last_idle_log >= 1000) {
      last_idle_log = idle_now;
      std::fprintf(stderr,
                   "host: send state viewer=%u last=%lld latest=%u has_image=%d "
                   "target_fps=%d viewer_paint=%.0f dropped=%d\n",
                   session->id, static_cast<long long>(last), latest->seq,
                   session->has_image ? 1 : 0, opt.capture->target_fps(),
                   session->viewer_paint_fps, session->viewer_dropped);
    }
  }
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
  uint32_t first = last < 0 ? static_cast<uint32_t>(start_seq)
                            : static_cast<uint32_t>(last) + 1;
  if (first > latest->seq) {
    send_empty_frame(transport, session, latest->width, latest->height, false);
    return;
  }
  // Tiles are changed pixels, not prediction. Skipping ahead is only for a
  // viewer that is actually behind (it reports drops, or the backlog is more
  // than a fraction of a second): doing it all the time made a link with
  // bandwidth to spare look choppy. A stream frame can never be skipped -- it
  // references the previous picture.
  const bool tiles = !opt.capture->stream_on();
  const bool viewer_behind = session->viewer_dropped >= 3;
  if (tiles && session->has_image && first + 1 <= latest->seq &&
      (viewer_behind || latest->seq - first > 8)) {
    first = latest->seq;
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
  if (std::getenv("MOLINK_STREAM_LOG") != nullptr) {
    static thread_local uint64_t last_action_log = 0;
    const uint64_t action_now = now_ms();
    if (action_now - last_action_log >= 1000) {
      last_action_log = action_now;
      std::fprintf(stderr,
                   "host: replay action=%d missing=%u oldest_age_us=%llu "
                   "last=%lld latest=%u\n",
                   static_cast<int>(action), missing,
                   static_cast<unsigned long long>(oldest_age_us),
                   static_cast<long long>(last), latest->seq);
    }
  }
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
    ask_keyframe();
    send_empty_frame(transport, session, latest->width, latest->height, false);
    return;
  }


  for (uint32_t seq = first; seq <= latest->seq; ++seq) {
    const std::shared_ptr<const captured_frame> frame = opt.capture->find(seq);
    if (!frame) {
      if (std::getenv("MOLINK_STREAM_LOG") != nullptr) {
        std::fprintf(stderr, "host: replay missing frame seq=%u\n", seq);
      }
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
      ask_keyframe();
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
      ask_keyframe();
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
      ask_keyframe();
      send_empty_frame(transport, session, frame->width, frame->height, false);
      return;
    }
    if (std::getenv("MOLINK_STREAM_LOG") != nullptr) {
      static thread_local uint64_t last_send_log = 0;
      static thread_local int stream_count = 0;
      static thread_local int tile_count = 0;
      static thread_local std::size_t last_size = 0;
      if (frame->av1_only) {
        ++stream_count;
        last_size = frame->av1_payload.size();
      } else {
        ++tile_count;
        last_size = frame->payload.size();
      }
      const uint64_t now_ms2 = now_ms();
      if (now_ms2 - last_send_log >= 1000) {
        last_send_log = now_ms2;
        std::fprintf(stderr,
                     "host: sent viewer=%u stream=%d tiles=%d last_bytes=%zu\n",
                     session->id, stream_count, tile_count, last_size);
        stream_count = 0;
        tile_count = 0;
      }
    }
    // stream_epoch_id: sent before the first frame of a new mode so the
    // viewer drops its old tile image / video reference queue and waits for
    // the keyframe that starts the epoch.
    if (frame->stream_epoch != session->stream_epoch) {
      session->stream_epoch = frame->stream_epoch;
      uvcpp_json epoch_msg;
      epoch_msg["type"] = "stream-epoch";
      epoch_msg["epoch"] = frame->stream_epoch;
      epoch_msg["mode"] = frame->av1_only ? "video" : "tiles";
      send_json(transport, epoch_msg);
    }
    if (!send_frame_with_av1(transport, session, *frame, &opt)) {
      // The transport refused the frame: its channel is full, or the session
      // is gone. Advancing anyway would tell the next tick that the viewer is
      // up to date when it is now one delta behind, and every later frame
      // would build on a picture it never applied. Start over from a keyframe
      // instead -- the sender's queue is part of the recovery loop.
      if (std::getenv("MOLINK_STREAM_LOG") != nullptr) {
        static uint64_t last_log_ms = 0;
        static int refused = 0;
        ++refused;
        const uint64_t now = now_ms();
        if (now - last_log_ms >= 1000) {
          std::fprintf(stderr,
                       "host: stream refused viewer=%u count=%d codec=%u "
                       "key=%d size=%zu seq=%u\n",
                       session->id, refused,
                       static_cast<unsigned>(frame->codec),
                       frame->keyframe ? 1 : 0, frame->av1_payload.size(), seq);
          last_log_ms = now;
          refused = 0;
        }
      }
      if (opt.capture) opt.capture->set_switch_inhibited(true);
      session->has_image = false;
      session->last_capture_seq = -1;
      ask_keyframe();
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
    // Toolbar order follows the preference order: AV1 > VP9 > VP8 > H.264.
    //
    // "tiles" is the explicit "never stream" choice and is the only entry in
    // tiles mode. Stream mode never shows it. Auto advertises both: the
    // stream encoders it may switch to and "tiles" for the still-screen
    // fallback.
    const std::string video_mode =
        opt.auth ? opt.auth->settings().video_mode : std::string("tiles");
    stream_codec_capabilities host_caps;
    host_caps.av1 = av1;
    host_caps.vp9 = vp9;
    host_caps.vp8 = vp8;
    host_caps.h264 = h264;
    stream_codec_capabilities viewer_caps;
    viewer_caps.av1 = session.av1_stream;
    viewer_caps.vp9 = session.vp9_stream;
    viewer_caps.vp8 = session.vp8_stream;
    viewer_caps.h264 = session.h264_stream;
    j["stream_codecs"] = uvcpp_json::array();
    for (const std::string& codec :
         advertised_stream_codecs(video_mode, host_caps, viewer_caps)) {
      j["stream_codecs"].push_back(codec);
    }
    // An explicit encoder that this host cannot run (or this viewer cannot
    // decode) is reported up front. The page shows the named encoder as
    // unavailable instead of letting the picture sit on tiles with no reason.
    const std::string preference =
        opt.auth ? opt.auth->settings().stream_codec : std::string("auto");
    bool codec_available = true;
    if (preference == "av1") {
      codec_available = host_caps.av1 && viewer_caps.av1;
    } else if (preference == "h264") {
      codec_available = host_caps.h264 && viewer_caps.h264;
    } else if (preference == "vp9") {
      codec_available = host_caps.vp9 && viewer_caps.vp9;
    } else if (preference == "vp8") {
      codec_available = host_caps.vp8 && viewer_caps.vp8;
    } else if (preference == "tiles" || preference == "auto") {
      codec_available = true;
    } else {
      codec_available = false;
    }
    j["codec_available"] = codec_available;
  }
  j["codec_preference"] =
      opt.auth ? opt.auth->settings().stream_codec : std::string("auto");
  // Host-global video path (stream / tiles). The native shell shows the same
  // switch the web's preferences panel offers.
  j["video_mode"] =
      opt.auth ? opt.auth->settings().video_mode : std::string("tiles");
  // The host's own avatar (a `preset:*` key or a small data URL), so a native
  // viewer can show the same picture the directory does.
  j["avatar"] = opt.auth ? opt.auth->settings().avatar : std::string();
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
  {
    std::vector<diag_field> fields;
    fields.push_back({"peer", session.peer_ip});
    fields.push_back({"session_id", session.public_id});
    fields.push_back({"proto", transport->name()});
    fields.push_back({"path", transport->path()});
    fields.push_back({"version", opt.version});
    diag().log(diag_level::info, "session.welcome", std::string(), fields);
  }
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
  // The toolbar sliders: their current values, so the page opens with what the
  // host is actually encoding instead of guessing from the preset.
  if (opt.capture) {
    const int rate_mode = opt.capture->stream_rate_mode_value();
    j["stream"]["qp"] = opt.capture->stream_qp();
    j["stream"]["bitrate_kbps"] = opt.capture->stream_bitrate_kbps();
    j["stream"]["rate_mode"] =
        rate_mode == 1 ? "cq" : (rate_mode == 2 ? "cbr" : "auto");
    j["stream"]["format"] = tile_format_name(opt.capture->quality_format());
    j["stream"]["format_custom"] = opt.capture->stream_format_override() >= 0;
    j["stream"]["fps"] = opt.capture->target_fps();
  }
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
  // The host can serve a viewer's `clipboard-pull` range requests: files the
  // host user copied can be pasted on the viewer's machine without staging
  // them there first.
  j["clipboard_pull"] = opt.clipboard != nullptr && opt.clipboard->available();
  const bool nested_rdp = opt.clipboard != nullptr && opt.clipboard->nested_rdp();
  j["clipboard_nested_rdp"] = nested_rdp;
  // Delayed file rendering: the viewer may send descriptors and let the host
  // fetch the bytes on the first paste. Clients that do not ask keep uploading
  // eagerly. The limits are advisory; the host enforces them too.
  j["clipboard_lazy"] =
      opt.clipboard != nullptr && opt.clipboard->supports_lazy_files();
  j["clipboard_lazy_files"] = static_cast<uint64_t>(kMaxLazyFiles);
  j["clipboard_lazy_bytes"] = static_cast<uint64_t>(kMaxLazyBytes);
  // Negotiated, not merely supported: both sides must be able to stream. A
  // client that did not advertise it in hello keeps the eager upload path.
  j["clipboard_stream"] = opt.clipboard != nullptr &&
                          opt.clipboard->supports_lazy_files() &&
                          session.clipboard_stream;
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

session_controller::~session_controller() = default;

void session_controller::update_file_retry_timer() {
  // `has_transfers` keeps the tick alive for a transfer that is simply waiting
  // for the viewer's next pull; otherwise an abandoned download would never
  // reach the idle reclaim in retry_pending().
  if (!files_->has_pending() && !files_->has_transfers()) {
    if (file_retry_timer_) file_retry_timer_->stop();
    return;
  }
  if (file_retry_timer_) {
    if (file_retry_timer_->is_active()) return;
  } else if (options_.loop) {
    auto timer = std::make_unique<uvcpp::uvcpp_timer>(options_.loop);
    if (timer->init() == 0) file_retry_timer_ = std::move(timer);
  }
  auto weak = weak_from_this();
  constexpr auto interval = file_transfer::kRetryIntervalMs;
  if (!file_retry_timer_ || file_retry_timer_->start(
          [weak](uvcpp::uvcpp_timer* timer) {
            if (auto self = weak.lock()) {
              self->files_->retry_pending();
              if (!self->files_->has_pending() &&
                  !self->files_->has_transfers()) {
                timer->stop();
              }
            }
          }, interval, interval) != 0) {
    files_->fail_pending();
  }
}

void session_controller::send_lazy_offer_reply(bool ok, bool stream,
                                               const std::string& message,
                                               bool compat) {
  std::shared_ptr<session_transport> transport = transport_.lock();
  if (!transport) return;
  uvcpp_json out;
  out["type"] = "file";
  out["action"] = "clipboard-offer";
  out["ok"] = ok;
  out["lazy"] = ok;
  // True when the host will fetch byte ranges instead of asking for an
  // upload. The page then keeps answering `fetch` and never uploads.
  out["stream"] = ok && stream;
  // Legacy escape hatch: an older host used this to say that an inner RDP
  // session could not render a delayed offer. Current hosts accept the offer
  // and stream ranges through the OLE data object even when mstsc owns the
  // clipboard, so `compat` is never sent any more; the field stays understood
  // by the client for compatibility with old cores.
  if (compat) out["compat"] = true;
  if (!ok) {
    out["message"] = message;
    diag().log(diag_level::warn, "clipboard.offer-rejected", message,
               {{"peer", transport->peer_ip()},
                {"mode", stream ? "stream" : "eager"}});
  }
  send_json(transport.get(), out);
}

void session_controller::request_lazy_range(std::size_t index, uint32_t id,
                                            uint64_t offset,
                                            uint32_t length) {
  std::shared_ptr<session_transport> transport = transport_.lock();
  std::shared_ptr<session_info> session = session_.lock();
  std::shared_ptr<remote_file_source> source = stream_source_;
  const auto fail = [&source](uint32_t request_id, uint64_t at) {
    if (source) source->note_range(request_id, 1, at, nullptr, 0);
  };
  // A posted request can outlive its read or its offer; do not ask the
  // viewer for bytes nothing is waiting on.
  if (!source || index >= source->size() || !source->has_pending(id)) {
    return;
  }
  if (!transport || !session || !session->authenticated || !options_.lazy ||
      !options_.lazy->active_for(session->id)) {
    fail(id, offset);
    return;
  }
  const lazy_clipboard_file& file = source->files()[index];
  uvcpp_json out;
  out["type"] = "file";
  out["action"] = "fetch";
  out["id"] = static_cast<uint64_t>(id);
  // `index` is not part of the essential contract (name would identify the
  // file), but two folders can contribute the same base name in one selection.
  out["index"] = static_cast<uint64_t>(index);
  out["name"] = file.name;
  out["offset"] = offset;
  out["length"] = static_cast<uint64_t>(length);
  send_json(transport.get(), out);
}

void session_controller::request_lazy_file(std::size_t index) {
  std::shared_ptr<session_transport> transport = transport_.lock();
  std::shared_ptr<session_info> session = session_.lock();
  std::shared_ptr<lazy_blob_source> source = lazy_source_;
  if (!source || index >= source->size()) {
    if (source) source->note_failed(index);
    return;
  }
  if (!transport || !session || !session->authenticated || !options_.lazy ||
      !options_.lazy->active_for(session->id)) {
    source->note_failed(index);
    return;
  }
  const lazy_clipboard_file& file = source->files()[index];
  uvcpp_json out;
  out["type"] = "file";
  out["action"] = "clipboard-fetch";
  out["index"] = static_cast<uint64_t>(index);
  out["tag"] = lazy_tag_base_ + static_cast<int>(index);
  out["name"] = file.name;
  out["size"] = file.size;
  send_json(transport.get(), out);
}

void session_controller::on_upload_event(int tag,
                                         file_transfer::upload_event event,
                                         uint32_t id, uint64_t received,
                                         uint64_t size,
                                         const std::string& path) {
  (void)id;
  (void)size;
  std::shared_ptr<lazy_blob_source> source = lazy_source_;
  if (!source) return;
  const int index = tag - lazy_tag_base_;
  if (index < 0 || static_cast<std::size_t>(index) >= source->size()) return;
  switch (event) {
    case file_transfer::upload_event::accepted:
      break;
    case file_transfer::upload_event::progress:
      source->note_progress(static_cast<std::size_t>(index), received);
      break;
    case file_transfer::upload_event::failed:
      diag().log(diag_level::warn, "clipboard.upload", "file failed",
                 {{"index", std::to_string(index)},
                  {"received", std::to_string(received)},
                  {"bytes", std::to_string(size)}});
      source->note_failed(static_cast<std::size_t>(index));
      break;
    case file_transfer::upload_event::done: {
      diag().log(diag_level::info, "clipboard.upload", "file rendered",
                 {{"index", std::to_string(index)},
                  {"bytes", std::to_string(received)},
                  {"path", path}});
      source->note_done(static_cast<std::size_t>(index), path);
      // The host is about to put a real path on CF_HDROP for this viewer.
      // Suppress the clipboard watcher so it is not announced straight back.
      if (!path.empty()) clipboard_mark_files_written({path});
      // Tell the originating viewer which host paths were materialised, so
      // its reverse clipboard sync suppresses the echo instead of downloading
      // its own files back (the source of the (1)(2) loop).
      std::shared_ptr<session_transport> transport = transport_.lock();
      if (transport && !path.empty()) {
        uvcpp_json out;
        out["type"] = "file";
        out["action"] = "clipboard-rendered";
        out["paths"] = uvcpp_json::array();
        out["paths"].push_back(path);
        send_json(transport.get(), out);
      }
      break;
    }
  }
}

bool session_controller::start(const std::string* initial_token) {
  std::shared_ptr<session_transport> transport = transport_.lock();
  if (!transport) return false;
  const std::string peer_ip = transport->peer_ip();
  {
    std::vector<diag_field> fields;
    fields.push_back({"peer", peer_ip});
    fields.push_back({"port", std::to_string(transport->peer_port())});
    fields.push_back({"proto", transport->name()});
    fields.push_back({"path", transport->path()});
    diag().log(diag_level::info, "session.connect", std::string(), fields);
  }

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
  files_->watch_uploads(
      [weak](int tag, file_transfer::upload_event event, uint32_t id,
             uint64_t received, uint64_t size, const std::string& path) {
        if (auto self = weak.lock()) {
          self->on_upload_event(tag, event, id, received, size, path);
        }
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
      diag().log(diag_level::info, "session.auth", "token accepted",
                 {{"peer", peer_ip}, {"method", "token"}, {"ok", "1"}});
      registry_->set_authenticated(session);
      if (options_.auth->valid_session(*initial_token)) {
        resume_token_ = *initial_token;
      } else {
        issue_resume_token();
      }
      if (options_.lock) options_.lock->record_success(peer_ip);
    } else if (!locked) {
      diag().log(diag_level::warn, "session.auth", "token rejected",
                 {{"peer", peer_ip}, {"method", "token"}, {"ok", "0"}});
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
    // Start the new session on the stream: the first full-screen tile
    // keyframe is the expensive one, and paying it made the first seconds
    // stutter. Ten seconds later the movement rule decides again.
    options_.capture->request_stream(10000);
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
    if (!announced_initial_files_) {
      announced_initial_files_ = true;
      announce_current_clipboard_files();
    }
    attach_chat();
    attach_call();
  }
}

void session_controller::announce_current_clipboard_files() {
  if (!options_.clipboard || !options_.clipboard->available()) return;
  const std::shared_ptr<session_info> session = session_.lock();
  if (!session || !session->authenticated) return;
  const uint32_t session_id = session->id;
  std::shared_ptr<session_registry> registry = registry_;
  auto post = options_.post;
  std::shared_ptr<clipboard_service> clipboard = options_.clipboard;
  options_.clipboard->request_files(
      [registry, post, clipboard, session_id](
          std::vector<clipboard_file> files) {
        files.erase(std::remove_if(files.begin(), files.end(),
                                   [](const clipboard_file& file) {
                                     return !clipboard_file_has_bytes(file) ||
                                            file.name.empty();
                                   }),
                    files.end());
        if (files.empty()) return;
        // This session gets the list directly, but the periodic watcher must
        // not announce the same unchanged list a moment later. Mark it as
        // published here; `send_clipboard_files_to` still runs even when it
        // was already published (a late joiner needs its own copy).
        clipboard_take_files_signature(clipboard_files_signature(files));
        const bool nested = clipboard && clipboard->nested_rdp();
        auto deliver = [registry, session_id, files = std::move(files),
                        nested] {
          const std::shared_ptr<session_info> live =
              registry->find(session_id);
          if (!live) return;
          send_clipboard_files_to(registry, *live, files, nested);
        };
        if (post) {
          post(std::move(deliver));
        } else {
          deliver();
        }
      });
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

void session_controller::send_clipboard_file_reply(uint32_t id,
                                                   uint32_t status,
                                                   uint64_t offset,
                                                   const uint8_t* data,
                                                   std::size_t len) {
  try {
  std::shared_ptr<session_transport> transport = transport_.lock();
  if (!transport || !transport->is_open()) return;
  std::vector<uint8_t> payload(16 + len);
  auto put_u32 = [&payload](std::size_t at, uint32_t value) {
    payload[at] = static_cast<uint8_t>(value & 0xff);
    payload[at + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
    payload[at + 2] = static_cast<uint8_t>((value >> 16) & 0xff);
    payload[at + 3] = static_cast<uint8_t>((value >> 24) & 0xff);
  };
  put_u32(0, id);
  put_u32(4, status);
  for (int i = 0; i < 8; ++i) {
    payload[8 + static_cast<std::size_t>(i)] =
        static_cast<uint8_t>((offset >> (8 * i)) & 0xff);
  }
  if (len != 0 && data != nullptr) {
    std::memcpy(payload.data() + 16, data, len);
  }
  std::vector<uint8_t> out;
  std::string err;
  if (!protocol::encode_packet_into(
          out, static_cast<uint8_t>(protocol::packet_type::clipboard_file), 0,
          0, 0, epoch_us(), payload.data(), payload.size(), err)) {
    return;
  }
  // File content, so it rides the file channel: a 256 KiB reply must not
  // head-of-line block input and control messages. When a WebRTC data channel
  // is up it has its own "file" channel; use it like an ordinary transfer.
  if (send_realtime_file(session_, out)) return;
  transport->send_binary(transport_channel::file, out.data(), out.size());
  } catch (const std::exception& error) {
    diag().log(diag_level::error, "clipboard.pull.error", error.what(),
               {{"id", std::to_string(id)}, {"where", "reply"}});
  } catch (...) {
    diag().log(diag_level::error, "clipboard.pull.error", "unknown exception",
               {{"id", std::to_string(id)}, {"where", "reply"}});
  }
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
  if (options_.capture) {
    options_.capture->set_active(true);
    options_.capture->request_stream(10000);
  }
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

  if (type == "logs") {
    // Reading the host's diagnostics is a privileged operation: the tail can
    // name peers and paths, even though credentials and clipboard content are
    // stripped before they are ever written.
    if (!require_auth()) return;
    const std::string action = string_field(j, "action");
    if (action == "tail") {
      int lines = 500;
      if (const auto it = j.find("lines");
          it != j.end() && it->is_number()) {
        lines = static_cast<int>(it->get<double>());
      }
      if (lines < 1) lines = 1;
      if (lines > 5000) lines = 5000;
      const std::string text = diag().tail(lines);
      uvcpp_json out;
      out["type"] = "logs";
      out["action"] = "tail";
      out["tail"] = text;
      out["size"] = static_cast<uint64_t>(text.size());
      out["lines"] = lines;
      send_json(transport.get(), out);
      return;
    }
    if (action == "info") {
      const diag_log::info info = diag().describe();
      uvcpp_json out;
      out["type"] = "logs";
      out["action"] = "info";
      out["enabled"] = info.enabled;
      out["directory"] = info.directory;
      out["retention_days"] = info.retention_days;
      out["current_bytes"] = info.current_bytes;
      out["file"] = info.current_file;
      send_json(transport.get(), out);
      return;
    }
    if (action == "enable" || action == "disable") {
      const bool on = action == "enable";
      if (!on) {
        // Record the switch itself before closing the file handle.
        diag().log(diag_level::info, "logs.state", "recording disabled",
                   {{"peer", peer_ip}});
      }
      if (options_.auth) options_.auth->set_diag_logs_enabled(on);
      diag().set_enabled(on);
      if (on) {
        diag().cleanup_old_files();
        diag().log(diag_level::info, "logs.state", "recording enabled",
                   {{"peer", peer_ip}});
      }
      uvcpp_json out;
      out["type"] = "logs";
      out["action"] = "state";
      out["enabled"] = on;
      send_json(transport.get(), out);
      return;
    }
    send_error(transport.get(), "bad_logs_action",
               "action must be tail, info, enable or disable");
    return;
  }

  if (type == "hello") {
    // Optional client identity; the shared chat uses it to say who typed a
    // message when several people are connected.
    const std::string client_name = string_field(j, "name");
    const std::string client_id = string_field(j, "device_id");
    if (!client_name.empty()) session->client_name = client_name.substr(0, 64);
    if (!client_id.empty()) session->client_id = client_id.substr(0, 64);
    // What this viewer can decode. Re-read on every hello, not just the first:
    // the page probes its decoders asynchronously and re-sends the hello when
    // a probe finishes, and a session approved by the host user is already
    // authenticated before its first hello arrives. Returning early without
    // this left the session claiming only H.264 forever.
    bool hello_av1_native = false;
    const auto read_hello_capabilities = [&] {
      session->av1 = bool_field(j, "av1", false);
      // The page names its stateful AV1 decoder `av1_native`; the local core's
      // proxy renames that to `av1_stream` while it sits in the path. The proxy
      // no longer decodes AV1 itself, so both names mean the same thing: this
      // viewer can take the low-delay AV1 stream.
      hello_av1_native = bool_field(j, "av1_native", false);
      session->av1_stream =
          bool_field(j, "av1_stream", false) || hello_av1_native;
      session->seq_clock = bool_field(j, "seq_clock", false);
      session->clipboard_stream = clipboard_stream_capable(j);
      session->clipboard_pull = bool_field(j, "clipboard_pull", false);
      session->video_push = bool_field(j, "video_push", false);
      session->h264_stream = bool_field(j, "h264_stream", false);
      session->vp9_stream = bool_field(j, "vp9_stream", false);
      session->vp8_stream = bool_field(j, "vp8_stream", false);
      session->webrtc = bool_field(j, "webrtc", false);
    };
    // A repeat hello must not cost another use of a multi-use code: the relay
    // proxy replays one when its first datagram races the session setup, and
    // a reconnect replays the whole handshake.
    if (session->authenticated) {
      read_hello_capabilities();
      if (std::getenv("MOLINK_STREAM_LOG") != nullptr) {
        std::fprintf(stderr,
                     "host: hello (repeat) av1_stream=%d h264=%d vp9=%d "
                     "vp8=%d\n",
                     session->av1_stream ? 1 : 0,
                     session->h264_stream ? 1 : 0,
                     session->vp9_stream ? 1 : 0,
                     session->vp8_stream ? 1 : 0);
      }
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
      diag().log(diag_level::warn, "session.auth", "hello rejected",
                 {{"peer", peer_ip},
                  {"method", resume.empty() ? "code" : "resume"},
                  {"ok", "0"},
                  {"locked", locked_for_peer ? "1" : "0"}});
      if (std::getenv("MOLINK_STREAM_LOG") != nullptr) {
        // Never the credential itself: only which one was presented, so a
        // replay that lost its code can be told from a wrong code.
        std::fprintf(stderr,
                     "host: auth failed peer=%s resume_len=%zu token_len=%zu "
                     "locked=%d\n",
                     peer_ip.c_str(), resume.size(),
                     string_field(j, "token").size(),
                     locked_for_peer ? 1 : 0);
      }
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
    diag().log(diag_level::info, "session.auth", "hello accepted",
               {{"peer", peer_ip},
                {"method", resume.empty() ? "code" : "resume"},
                {"ok", "1"},
                {"locked", locked_for_peer ? "1" : "0"}});
    read_hello_capabilities();
    if (std::getenv("MOLINK_AV1_LOG") != nullptr) {
      std::fprintf(stderr, "host: session av1=%d\n", session->av1 ? 1 : 0);
    }
    // MOLINK_STREAM_LOG=1: what this viewer can decode, so "the host sends
    // tiles" can be told apart from "the page never asked for a stream".
    if (std::getenv("MOLINK_STREAM_LOG") != nullptr) {
      std::fprintf(stderr,
                   "host: hello av1=%d av1_stream=%d av1_native=%d "
                   "h264_stream=%d webrtc=%d video_push=%d vp9=%d vp8=%d\n",
                   session->av1 ? 1 : 0, session->av1_stream ? 1 : 0,
                   hello_av1_native ? 1 : 0, session->h264_stream ? 1 : 0,
                   session->webrtc ? 1 : 0, session->video_push ? 1 : 0,
                   session->vp9_stream ? 1 : 0,
                   session->vp8_stream ? 1 : 0);
    }
    registry_->set_authenticated(session);
    // A session without AV1 must stop patches immediately; the supervisor
    // turns them back on once every session can decode them again.
    if (!session->av1 && options_.capture) options_.capture->set_av1_allowed(false);
    if (locked_for_peer) options_.lock->unlock();
    if (options_.lock) options_.lock->record_success(peer_ip);
    if (options_.capture) {
      options_.capture->set_active(true);
      // A viewer that just joined starts on the stream, not on tiles.
      options_.capture->request_stream(10000);
    }
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
    // A no-code session never sends a hello, so this request carries the one
    // capability the clipboard path needs.
    session->clipboard_stream = clipboard_stream_capable(j);
    session->clipboard_pull = bool_field(j, "clipboard_pull", false);
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
    double rtt = 0;
    number_field(j, "fps", fps);
    number_field(j, "dropped", dropped);
    number_field(j, "rx", rx);
    number_field(j, "rtt", rtt);
    session->viewer_paint_fps = fps < 0 ? 0 : fps;
    session->viewer_dropped = dropped < 0 ? 0 : static_cast<int>(dropped);
    session->viewer_rx_rate = rx < 0 ? 0 : rx;
    session->viewer_rtt_ms = rtt < 0 ? 0 : rtt;
    session->viewer_stats_ms = now_ms();
    // A viewer reporting a slow round trip is the earliest sign of a bad
    // relay/P2P path. One line per ten seconds per host keeps it useful.
    if (rtt > 500) {
      static std::atomic<uint64_t> last_rtt_log_ms{0};
      const uint64_t now = now_ms();
      uint64_t previous = last_rtt_log_ms.load();
      if (now > previous + 10000 &&
          last_rtt_log_ms.compare_exchange_strong(previous, now)) {
        diag().log(diag_level::warn, "transport.rtt", "viewer round trip high",
                   {{"peer", peer_ip},
                    {"rtt_ms", std::to_string(static_cast<int>(rtt))},
                    {"fps", std::to_string(static_cast<int>(fps))},
                    {"dropped", std::to_string(static_cast<int>(dropped))}});
      }
    }
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

  // Viewer-owned picture preset. The viewer sends this after the welcome
  // (before the first frame) and on every change. It is deliberately not
  // written to auth settings and not broadcast to other viewers: the capture
  // is shared, but each viewer's selector stays its own.
  if (type == "session-preset") {
    if (!options_.capture) {
      send_error(transport.get(), "not_implemented", "capture is unavailable");
      return;
    }
    session_preset_request request;
    request.preset = string_field(j, "preset");
    request.rate_mode = string_field(j, "rate_mode");
    request.format = string_field(j, "format");
    request.chroma = string_field(j, "chroma");
    request.codec = string_field(j, "codec");
    const auto read_int = [&j](const char* name, int& out) {
      const auto it = j.find(name);
      if (it == j.end()) return true;
      if (!it->is_number()) return false;
      out = static_cast<int>(it->get<double>());
      return true;
    };
    if (!read_int("fps", request.fps) ||
        !read_int("quality", request.quality) ||
        !read_int("bitrate_kbps", request.bitrate_kbps)) {
      send_error(transport.get(), "bad_preset",
                 "fps, quality and bitrate_kbps must be numbers");
      return;
    }
    session_preset_request resolved;
    std::string error;
    session_preset_capabilities caps;
    // Probe the encoders rather than assuming: 4:4:4 stays on the video
    // stream only when some available backend can actually carry it. With
    // today's 4:2:0 encoders normalize resolves such a request to 4:2:0
    // instead of silently disabling the stream (which is what pushed "高清"
    // onto tiles).
    caps.stream_444 = video_stream_supports_444();
    if (!normalize_session_preset(request, resolved, error, caps)) {
      send_error(transport.get(), "bad_preset", error);
      return;
    }
    // A `custom` request may name the encoder itself. The host runs one
    // encoder at a time, so honouring it is the same operation as the
    // toolbar's `stream-codec` message: pin the host preference and apply it
    // to the capture immediately. `auto` leaves the existing preference
    // alone (the advanced panel did not choose an encoder).
    std::string preference =
        options_.auth ? options_.auth->settings().stream_codec : "auto";
    bool codec_changed = false;
    if (resolved.preset == "custom" && resolved.codec != "auto") {
      bool ok = resolved.codec == "tiles";
      if (!ok) {
        for (const video_encoder_backend& backend : video_encoder_backends()) {
          if (!backend.available) continue;
          if ((resolved.codec == "av1" && backend.av1_stream) ||
              (resolved.codec == "h264" && backend.h264_stream) ||
              (resolved.codec == "vp9" && backend.vp9_stream) ||
              (resolved.codec == "vp8" && backend.vp8_stream)) {
            ok = true;
            break;
          }
        }
      }
      if (!ok) {
        // The rest of the request (rate, QP, bitrate) is still valid. Keep it
        // and report the encoder that is actually running instead of
        // rejecting the whole picture change: the panel shows the real value
        // in its "applied" line, and a later change can pick another codec.
        diag().log(diag_level::warn, "quality.preset.codec_unavailable",
                   std::string(),
                   {{"peer", peer_ip}, {"codec", resolved.codec}});
      } else {
        if (options_.auth) options_.auth->set_stream_codec(resolved.codec);
        preference = resolved.codec;
        codec_changed = true;
        if (options_.capture) {
          const std::string mode =
              options_.auth ? options_.auth->settings().video_mode : "auto";
          options_.capture->set_stream_always(
              stream_always_for(mode, resolved.codec));
          options_.capture->set_stream_locked(
              stream_locked_for(resolved.codec));
          options_.capture->set_av1_stream_allowed(resolved.codec == "av1");
          options_.capture->set_h264_stream_allowed(resolved.codec == "h264");
          options_.capture->set_vp9_stream_allowed(resolved.codec == "vp9");
          options_.capture->set_vp8_stream_allowed(resolved.codec == "vp8");
          if (resolved.codec != "tiles") {
            options_.capture->request_stream(15000);
          }
        }
        // The encoder is host-global; every tab's codec selector has to show
        // the value the advanced request just pinned.
        uvcpp_json shared;
        shared["type"] = "stream-codec";
        shared["value"] = resolved.codec;
        shared["scope"] = "global";
        for (const session_info& peer : registry_->snapshot()) {
          if (!peer.authenticated || !peer.transport) continue;
          send_json(peer.transport.get(), shared);
        }
      }
    }
    // An explicitly pinned encoder is 4:2:0. The truecolour tier asks for
    // 4:4:4 and the tile path, but the user asked for VP9 (or another
    // encoder) by name: keep the stream and reduce the colour depth. The
    // viewer's UI states this instead of the stream silently staying off.
    const bool explicit_codec = is_explicit_stream_codec(preference);
    if (explicit_codec) {
      resolved.chroma = "420";
      resolved.no_subsampling = false;
    }
    // The first preset of a session always starts from a keyframe, and a
    // preset/format/rate change rebuilds the encoder at the next frame
    // boundary (the capture thread owns it, so no half-encoded frame is
    // published). A pure fps change does not need one.
    const bool first = session->encoding_preset.empty();
    const bool reinit =
        session_preset_requires_keyframe(*options_.capture, resolved);
    // Reset every field the tier owns. This is the auto-after-truecolor fix:
    // no_subsampling is assigned from this request, never inherited, and the
    // capture's format override is re-checked against it.
    const session_preset_actual actual =
        apply_session_preset(*options_.capture, *session, resolved);
    if (first || reinit || codec_changed) options_.capture->request_keyframe();
    // A preset that may use the video stream nudges it on for a window, just
    // like the join window. Without the nudge a static screen kept the
    // capture on tiles, so switching back from truecolor to auto reported
    // "tiles 4:4:4" until something moved.
    const std::string video_mode =
        options_.auth ? options_.auth->settings().video_mode : "tiles";
    const bool stream_permitted =
        explicit_codec || (video_mode != "tiles" && preference != "tiles");
    if (!resolved.no_subsampling && stream_permitted) {
      // Ten seconds, the same window a joining viewer gets.
      constexpr uint64_t kPresetStreamMs = 10000;
      options_.capture->request_stream(kPresetStreamMs);
    }

    uvcpp_json ack;
    ack["type"] = "session-preset";
    ack["applied"] = true;
    ack["preset"] = resolved.preset;
    const std::string stream_codec = options_.capture->stream_codec_name();
    const bool streaming = stream_codec != "tiles";
    ack["actual"]["fps"] = actual.fps;
    ack["actual"]["quality"] = actual.quality;
    ack["actual"]["format"] = actual.format;
    // A stream is 4:2:0 unless the host has (and kept) a 4:4:4 encoder.
    ack["actual"]["chroma"] =
        streaming ? (resolved.chroma == "444" ? "444" : "420") : actual.chroma;
    ack["actual"]["codec"] = stream_codec;
    {
      std::vector<diag_field> fields;
      fields.push_back({"peer", peer_ip});
      fields.push_back({"preset", resolved.preset});
      fields.push_back({"fps", std::to_string(actual.fps)});
      fields.push_back({"quality", actual.quality});
      fields.push_back({"format", actual.format});
      fields.push_back({"codec", stream_codec});
      fields.push_back({"chroma", ack["actual"]["chroma"].get<std::string>()});
      fields.push_back({"encoder_rebuild", reinit ? "1" : "0"});
      fields.push_back({"first", first ? "1" : "0"});
      diag().log(diag_level::info, "quality.preset", std::string(), fields);
    }
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
      // The preset is per viewer now; other sessions keep their own selection
      // and are not told what this one picked.
      options_.capture->request_keyframe();
    }
    int fps = 0;
    if (fps_it != j.end()) {
      fps = apply_fps(static_cast<int>(fps_it->get<double>()));
    }

    // Toolbar sliders. A field that is absent keeps the host's current value;
    // 0 (or "preset") means "follow the quality preset again".
    int qp = options_.capture->stream_qp();
    int bitrate_kbps = options_.capture->stream_bitrate_kbps();
    int rate_mode = options_.capture->stream_rate_mode_value();
    int format = options_.capture->stream_format_override();
    const auto qp_it = j.find("qp");
    if (qp_it != j.end()) {
      if (!qp_it->is_number() || qp_it->get<double>() < 0 ||
          qp_it->get<double>() > 63) {
        send_error(transport.get(), "bad_qp", "qp must be 0..63");
        return;
      }
      qp = static_cast<int>(qp_it->get<double>());
    }
    const auto bitrate_it = j.find("bitrate_kbps");
    if (bitrate_it != j.end()) {
      if (!bitrate_it->is_number() || bitrate_it->get<double>() < 0 ||
          bitrate_it->get<double>() > 60000) {
        send_error(transport.get(), "bad_bitrate",
                   "bitrate_kbps must be 0..60000");
        return;
      }
      bitrate_kbps = static_cast<int>(bitrate_it->get<double>());
    }
    std::string rate_name = string_field(j, "rate_mode");
    if (!rate_name.empty()) {
      if (rate_name == "auto") {
        rate_mode = 0;
      } else if (rate_name == "cq") {
        rate_mode = 1;
      } else if (rate_name == "cbr") {
        rate_mode = 2;
      } else {
        send_error(transport.get(), "bad_rate_mode",
                   "rate_mode must be auto, cq or cbr");
        return;
      }
    }
    std::string format_name = string_field(j, "format");
    if (!format_name.empty()) {
      if (format_name == "preset") {
        format = -1;
      } else {
        tile_format parsed;
        if (!tile_format_from_name(format_name, parsed)) {
          send_error(transport.get(), "bad_format",
                     "format must be gray8, rgb565, rgb24, bgra or preset");
          return;
        }
        format = static_cast<int>(parsed);
      }
    }
    const bool knobs_changed =
        qp != options_.capture->stream_qp() ||
        bitrate_kbps != options_.capture->stream_bitrate_kbps() ||
        rate_mode != options_.capture->stream_rate_mode_value() ||
        format != options_.capture->stream_format_override();
    options_.capture->set_stream_overrides(qp, bitrate_kbps, rate_mode, format);
    // Deliberately not persisted through auth->set_stream_overrides: these
    // are the viewer's live knobs and must not become the host's settings.
    if (knobs_changed || options_.capture->quality() != previous) {
      // A new format or rate mode re-initialises the encoder and the client
      // must paint from a keyframe.
      options_.capture->request_keyframe();
    }
    const tile_format effective_format = options_.capture->quality_format();
    uvcpp_json ack;
    ack["type"] = "quality";
    ack["mode"] = quality_mode_name(options_.capture->quality());
    ack["format"] = tile_format_name(effective_format);
    ack["qp"] = options_.capture->stream_qp();
    ack["bitrate_kbps"] = options_.capture->stream_bitrate_kbps();
    ack["rate_mode"] = options_.capture->stream_rate_mode_value() == 1
                           ? "cq"
                           : (options_.capture->stream_rate_mode_value() == 2
                                  ? "cbr"
                                  : "auto");
    ack["scope"] = "viewer";
    if (fps != 0) ack["fps"] = fps;
    {
      std::vector<diag_field> fields;
      fields.push_back({"peer", peer_ip});
      fields.push_back({"mode", quality_mode_name(options_.capture->quality())});
      fields.push_back({"format", tile_format_name(effective_format)});
      fields.push_back({"qp", std::to_string(options_.capture->stream_qp())});
      fields.push_back(
          {"kbps", std::to_string(options_.capture->stream_bitrate_kbps())});
      fields.push_back({"rate", ack["rate_mode"].get<std::string>()});
      fields.push_back({"changed", knobs_changed ? "1" : "0"});
      if (fps != 0) fields.push_back({"fps", std::to_string(fps)});
      diag().log(diag_level::info, "quality.apply", std::string(), fields);
    }
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
    clipboard_reset_files_signature();
    {
      // Length only: the text itself never reaches the log.
      std::vector<diag_field> fields;
      fields.push_back({"peer", peer_ip});
      fields.push_back({"bytes", std::to_string(text.size())});
      diag().log(diag_level::info, "clipboard.sync", std::string(), fields);
    }
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
            diag().log(diag_level::info, "clipboard.files",
                       std::string(),
                       {{"count", std::to_string(files.size())}});
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
      diag().log(diag_level::info, "clipboard.set", std::string(),
                 {{"peer", peer_ip},
                  {"bytes", std::to_string(text_value.size())}});
      options_.clipboard->set_text(std::move(text_value));
      clipboard_reset_files_signature();
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
              diag().log(diag_level::info, "clipboard.get", std::string(),
                         {{"bytes", std::to_string(clipboard_text.size())}});
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

  // Host-global video path, the session-scoped twin of POST /api/settings
  // `video_mode`: the native shell has no HTTP client and the web's settings
  // page is not available to it, so the mode travels over the same socket the
  // picture does.
  if (type == "video-mode") {
    if (!require_auth()) return;
    const std::string value = string_field(j, "value");
    if (!options_.auth || !options_.auth->set_video_mode(value)) {
      send_error(transport.get(), "bad_video_mode",
                 "expected auto, stream or tiles");
      return;
    }
    // Broadcast the normalised mode: an older client may still send "mixed",
    // which is the adaptive path now.
    const std::string mode = normalize_video_mode(value);
    if (options_.capture) {
      // "stream" pins the video path and "tiles" disables it; "auto" leaves
      // stream_always_ false so the supervisor's stream_policy decides. A
      // tiles codec preference still wins over the stream mode: it is the
      // viewer's manual choice from the toolbar. The supervisor applies the
      // rest on its next tick.
      const std::string preference = options_.auth->settings().stream_codec;
      options_.capture->set_stream_always(stream_always_for(mode, preference));
      options_.capture->set_stream_locked(stream_locked_for(preference));
    }
    uvcpp_json shared;
    shared["type"] = "video-mode";
    shared["value"] = mode;
    shared["scope"] = "global";
    for (const session_info& peer : registry_->snapshot()) {
      if (!peer.authenticated || !peer.transport) continue;
      send_json(peer.transport.get(), shared);
    }
    return;
  }

  if (type == "stream-codec") {
    if (!require_auth()) return;
    const std::string value = string_field(j, "value");
    // Only codecs this host can actually encode are accepted, plus "auto"
    // and the explicit "tiles" (never stream).
    bool ok = value == "auto" || value == "tiles";
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
    // A codec (or path) switch has to be visible at once. Apply it to the
    // capture here instead of waiting for the supervisor's one-second tick;
    // the supervisor still re-reads the preference every tick, so the change
    // takes effect without reconnecting and is corrected if another viewer
    // cannot decode a codec. Picking "tiles" is a request to stop streaming,
    // so it releases stream_always_ and skips the stream nudge.
    if (options_.capture) {
      const std::string mode = options_.auth->settings().video_mode;
      options_.capture->set_stream_always(stream_always_for(mode, value));
      // The explicit encoder locks the stream path immediately. The
      // supervisor re-checks every viewer's decoder and clears the lock if
      // one of them cannot take it.
      options_.capture->set_stream_locked(stream_locked_for(value));
      if (value != "auto") {
        options_.capture->set_av1_stream_allowed(value == "av1");
        options_.capture->set_h264_stream_allowed(value == "h264");
        options_.capture->set_vp9_stream_allowed(value == "vp9");
        options_.capture->set_vp8_stream_allowed(value == "vp8");
      }
      if (value != "tiles") options_.capture->request_stream(15000);
    }
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
    // A keyframe costs the capture a full-screen encode and is broadcast to
    // every viewer, so one struggling decoder must not turn the whole host
    // into a keyframe pump. The client already throttles to 300 ms; this is
    // the same one-per-second budget the replay path uses.
    const uint64_t now = now_ms();
    if (session->last_keyframe_request_ms != 0 &&
        now - session->last_keyframe_request_ms < 1000) {
      return;
    }
    session->last_keyframe_request_ms = now;
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
    if (action == "clipboard-pull") {
      // A viewer is downloading a file that was copied on this host (possibly
      // inside a nested RDP session). Re-enumerate CF_HDROP *now*: RDP
      // redirection replaces its temporary files, so a path cached at
      // announcement time may already be gone.
      uint64_t offset = 0;
      uint64_t length = kClipboardPullChunkBytes;
      if (const auto it = j.find("offset");
          it != j.end() && it->is_number()) {
        const double value = it->get<double>();
        if (value >= 0) offset = static_cast<uint64_t>(value);
      }
      if (const auto it = j.find("length");
          it != j.end() && it->is_number()) {
        const double value = it->get<double>();
        if (value > 0) length = static_cast<uint64_t>(value);
      }
      if (length > kClipboardPullChunkBytes) length = kClipboardPullChunkBytes;
      const uint32_t file_id = id;
      const uint64_t pull_started_ms = now_ms();
      {
        std::vector<diag_field> fields;
        fields.push_back({"peer", peer_ip});
        fields.push_back({"id", std::to_string(file_id)});
        fields.push_back({"offset", std::to_string(offset)});
        fields.push_back({"length", std::to_string(length)});
        diag().log(diag_level::info, "clipboard.pull.start", std::string(),
                   fields);
      }
      if (!options_.clipboard || !options_.clipboard->available()) {
        send_clipboard_file_reply(
            file_id, static_cast<uint32_t>(clipboard_pull_status::gone),
            offset, nullptr, 0);
        return;
      }
      const uint32_t session_id = session->id;
      std::weak_ptr<session_controller> weak = weak_from_this();
      auto post = options_.post;
      auto registry_ref = registry_;
      std::shared_ptr<clipboard_service> clipboard = options_.clipboard;
      // The clipboard backend calls this on its own thread. A throw there
      // would unwind into a COM/clipboard worker or the event loop and take
      // the whole core down; answer the viewer with a read error instead.
      const auto reply_failure = [weak, file_id, offset]() noexcept {
        try {
          if (auto self = weak.lock()) {
            self->send_clipboard_file_reply(
                file_id, static_cast<uint32_t>(clipboard_pull_status::read_error),
                offset, nullptr, 0);
          }
        } catch (...) {
          // Nothing left to answer with; the log below is the only trace.
        }
      };
      const auto receive_files =
          [weak, post, registry_ref, clipboard, session_id, file_id, offset,
           length, pull_started_ms,
           reply_failure](std::vector<clipboard_file> current) {
        try {
          const std::vector<clipboard_offer_entry> offered =
              registry_ref->clipboard_offer(session_id);
          clipboard_pull_result result = read_clipboard_pull(
              current, offered, file_id, offset, length,
              kClipboardPullChunkBytes);
          if (result.status == clipboard_pull_status::ok && result.stream) {
            // An inner RDP virtual file: the bytes are behind the OLE data
            // object. read_clipboard_pull already validated the id, name and
            // size; the backend reads the range on its worker thread so a
            // blocked RDP channel cannot stall this loop.
            if (file_id == 0 || file_id > current.size()) {
              reply_failure();
              return;
            }
            const clipboard_file target = current[file_id - 1];
            const uint64_t reply_offset = result.offset;
            const uint64_t take = result.length;
            try {
              clipboard->read_file_range(
                  target, reply_offset, take,
                  [weak, post, file_id, pull_started_ms, reply_failure](
                      uint32_t status, uint64_t reply_at,
                      std::vector<uint8_t> data) {
                    try {
                      auto deliver = [weak, file_id, status, reply_at,
                                      pull_started_ms,
                                      data = std::move(data)] {
                        if (auto self = weak.lock()) {
                          std::vector<diag_field> fields;
                          fields.push_back({"id", std::to_string(file_id)});
                          fields.push_back({"status", std::to_string(status)});
                          fields.push_back(
                              {"bytes", std::to_string(data.size())});
                          fields.push_back(
                              {"offset", std::to_string(reply_at)});
                          fields.push_back(
                              {"ms", std::to_string(now_ms() -
                                                    pull_started_ms)});
                          fields.push_back({"mode", "stream"});
                          diag().log(diag_level::info, "clipboard.pull.done",
                                     std::string(), fields);
                          self->send_clipboard_file_reply(
                              file_id, status, reply_at,
                              data.empty() ? nullptr : data.data(),
                              data.size());
                        }
                      };
                      if (post) {
                        post(std::move(deliver));
                      } else {
                        deliver();
                      }
                    } catch (const std::exception& error) {
                      diag().log(diag_level::error, "clipboard.pull.error",
                                 error.what(),
                                 {{"id", std::to_string(file_id)},
                                  {"where", "read_file_range"}});
                      reply_failure();
                    } catch (...) {
                      diag().log(diag_level::error, "clipboard.pull.error",
                                 "unknown exception",
                                 {{"id", std::to_string(file_id)},
                                  {"where", "read_file_range"}});
                      reply_failure();
                    }
                  });
            } catch (const std::exception& error) {
              diag().log(diag_level::error, "clipboard.pull.error",
                         error.what(),
                         {{"id", std::to_string(file_id)},
                          {"where", "read_file_range-start"}});
              reply_failure();
            } catch (...) {
              diag().log(diag_level::error, "clipboard.pull.error",
                         "unknown exception",
                         {{"id", std::to_string(file_id)},
                          {"where", "read_file_range-start"}});
              reply_failure();
            }
            return;
          }
          auto deliver = [weak, file_id, pull_started_ms,
                          result = std::move(result)] {
            if (auto self = weak.lock()) {
              std::vector<diag_field> fields;
              fields.push_back({"id", std::to_string(file_id)});
              fields.push_back(
                  {"status",
                   std::to_string(static_cast<uint32_t>(result.status))});
              fields.push_back(
                  {"bytes", std::to_string(result.data.size())});
              fields.push_back({"offset", std::to_string(result.offset)});
              fields.push_back(
                  {"ms", std::to_string(now_ms() - pull_started_ms)});
              fields.push_back({"mode", result.stream ? "range" : "eager"});
              diag().log(diag_level::info, "clipboard.pull.done",
                         std::string(), fields);
              self->send_clipboard_file_reply(
                  file_id, static_cast<uint32_t>(result.status),
                  result.offset,
                  result.data.empty() ? nullptr : result.data.data(),
                  result.data.size());
            }
          };
          if (post) {
            post(std::move(deliver));
          } else {
            deliver();
          }
        } catch (const std::exception& error) {
          diag().log(diag_level::error, "clipboard.pull.error", error.what(),
                     {{"id", std::to_string(file_id)},
                      {"where", "request_files"}});
          reply_failure();
        } catch (...) {
          diag().log(diag_level::error, "clipboard.pull.error",
                     "unknown exception",
                     {{"id", std::to_string(file_id)},
                      {"where", "request_files"}});
          reply_failure();
        }
      };
      try {
        clipboard->request_files(receive_files);
      } catch (const std::exception& error) {
        diag().log(diag_level::error, "clipboard.pull.error", error.what(),
                   {{"id", std::to_string(file_id)},
                    {"where", "request_files-start"}});
        reply_failure();
      } catch (...) {
        diag().log(diag_level::error, "clipboard.pull.error",
                   "unknown exception",
                   {{"id", std::to_string(file_id)},
                    {"where", "request_files-start"}});
        reply_failure();
      }
      return;
    }
    if (action == "offer") {
      uint64_t size = 0;
      if (const auto it = j.find("size");
          it != j.end() && it->is_number()) {
        size = it->get<uint64_t>();
      }
      // The delayed clipboard path tags the upload it asked for. An ordinary
      // offer carries no tag and the listener ignores it.
      int tag = -1;
      if (const auto it = j.find("clipboard_tag");
          it != j.end() && it->is_number()) {
        tag = it->get<int>();
      }
      diag().log(diag_level::info, "file.offer", std::string(),
                 {{"peer", peer_ip},
                  {"name", string_field(j, "name")},
                  {"bytes", std::to_string(size)},
                  {"dest", string_field(j, "dest")}});
      files_->offer(string_field(j, "name"), size, string_field(j, "dest"),
                    tag);
      update_file_retry_timer();
    } else if (action == "clipboard-offer") {
      // The viewer copied files on its own machine. Register the descriptors
      // only; the bytes are fetched when a remote application first pastes.
      if (!options_.lazy || !options_.clipboard ||
          !options_.clipboard->supports_lazy_files()) {
        send_lazy_offer_reply(false, false,
                              "this host cannot render files on demand");
        return;
      }
      std::vector<lazy_clipboard_file> offered;
      const auto files_it = j.find("files");
      if (files_it == j.end() || !files_it->is_array()) {
        send_lazy_offer_reply(false, false, "files must be an array");
        return;
      }
      // Cap the parse before the validator reports the real reason, so a huge
      // array cannot make this loop the expensive part.
      for (const auto& item : *files_it) {
        if (offered.size() >= kMaxLazyFiles) break;
        if (!item.is_object()) {
          send_lazy_offer_reply(false, false,
                                "each file must be an object");
          return;
        }
        lazy_clipboard_file file;
        file.name = string_field(item, "name");
        if (const auto size_it = item.find("size");
            size_it != item.end() && size_it->is_number()) {
          file.size = size_it->get<uint64_t>();
        }
        offered.push_back(std::move(file));
      }
      // `source` posts to this session's event loop; the clipboard thread
      // then waits on its condition variable, never on the network. A viewer
      // that advertised `clipboard_stream` gets a range source, so a paste
      // writes Explorer's target file directly and nothing is staged under
      // ~/moLink. Everyone else keeps the upload-then-CF_HDROP path.
      //
      // A nested RDP session is *not* a reason to materialise the file on
      // this host. mstsc advertises FileGroupDescriptorW/FileContents and
      // answers FILECONTENTS_RANGE, so the virtual stream is the path that
      // reaches the inner desktop without a host-side copy. Forcing the eager
      // source here made every nested copy download in full before the paste
      // could start, and left a half-written staging file when it failed.
      // MOLINK_RDP_STREAM_PUSH=0 is an escape hatch for a real machine whose
      // inner client refuses the stream.
      bool stream = session->clipboard_stream;
      if (stream && options_.clipboard->nested_rdp()) {
        const char* eager = std::getenv("MOLINK_RDP_STREAM_PUSH");
        if (eager != nullptr && std::string(eager) == "0") {
          stream = false;
        }
      }
      std::weak_ptr<session_controller> weak = weak_from_this();
      auto post = options_.post;
      std::shared_ptr<lazy_blob_source> source;
      std::shared_ptr<remote_file_source> new_stream;
      if (stream) {
        new_stream = std::make_shared<remote_file_source>(
            offered, [weak, post](std::size_t index, uint32_t id,
                                  uint64_t offset, uint32_t length) {
              if (weak.expired()) return false;
              auto deliver = [weak, index, id, offset, length] {
                if (auto self = weak.lock()) {
                  self->request_lazy_range(index, id, offset, length);
                }
              };
              if (post) {
                post(std::move(deliver));
              } else {
                deliver();
              }
              return true;
            });
        source = new_stream;
      } else {
        source = std::make_shared<lazy_file_source>(
            offered, [weak, post](std::size_t index) {
              auto deliver = [weak, index] {
                if (auto self = weak.lock()) self->request_lazy_file(index);
              };
              if (post) {
                post(std::move(deliver));
              } else {
                deliver();
              }
            });
      }
      const lazy_offer_error error = options_.lazy->offer(
          session->id, string_field(j, "dest"), offered, source);
      if (error != lazy_offer_error::none) {
        // The previous offer (if any) is still live; leave its routing alone.
        send_lazy_offer_reply(
            false, stream,
            error == lazy_offer_error::too_many_files
                ? "at most 8 files per clipboard offer"
                : (error == lazy_offer_error::too_large
                       ? "clipboard files exceed 2 GiB"
                       : "invalid clipboard file list"));
        return;
      }
      lazy_source_ = std::move(source);
      if (stream) {
        stream_source_ = std::move(new_stream);
      } else {
        stream_source_.reset();
        // Each eager offer owns a tag range; a late upload from the previous
        // offer lands outside this range and is ignored.
        if (lazy_tag_base_ > (1 << 24)) lazy_tag_base_ = 0;
        lazy_tag_base_ += kLazyTagStride;
      }
      std::string install_error;
      if (!options_.clipboard->install_lazy(options_.lazy, install_error)) {
        options_.lazy->clear_session(session->id);
        lazy_source_.reset();
        stream_source_.reset();
        send_lazy_offer_reply(false, stream, install_error);
        return;
      }
      {
        uint64_t total = 0;
        for (const lazy_clipboard_file& file : offered) total += file.size;
        std::vector<diag_field> fields;
        fields.push_back({"peer", peer_ip});
        fields.push_back({"files", std::to_string(offered.size())});
        fields.push_back({"bytes", std::to_string(total)});
        fields.push_back({"dest", string_field(j, "dest")});
        fields.push_back({"mode", stream ? "stream" : "eager"});
        fields.push_back(
            {"nested_rdp",
             options_.clipboard && options_.clipboard->nested_rdp() ? "1"
                                                                   : "0"});
        diag().log(diag_level::info, "clipboard.offer", std::string(), fields);
      }
      send_lazy_offer_reply(true, stream, std::string());
    } else if (action == "clipboard-fetch-failed") {
      // The viewer could not read the local file any more (the clipboard
      // changed). Fail that file at once instead of waiting out the timeout.
      uint64_t index = 0;
      if (const auto it = j.find("index");
          it != j.end() && it->is_number()) {
        index = it->get<uint64_t>();
      }
      diag().log(diag_level::warn, "clipboard.fetch-failed", std::string(),
                 {{"peer", peer_ip}, {"index", std::to_string(index)}});
      if (lazy_source_ && index < lazy_source_->size()) {
        lazy_source_->note_failed(static_cast<std::size_t>(index));
      }
    } else if (action == "mkdir") {
      files_->mkdir(string_field(j, "path"), string_field(j, "dest"));
    } else if (action == "complete") {
      diag().log(diag_level::info, "file.complete", std::string(),
                 {{"peer", peer_ip}, {"id", std::to_string(id)}});
      files_->complete(id);
      update_file_retry_timer();
    } else if (action == "cancel") {
      diag().log(diag_level::info, "file.cancel", std::string(),
                 {{"peer", peer_ip}, {"id", std::to_string(id)}});
      files_->cancel(id);
      update_file_retry_timer();
    } else if (action == "browse") {
      bool roots = false;
      if (const auto it = j.find("roots");
          it != j.end() && it->is_boolean()) {
        roots = it->get<bool>();
      }
      files_->browse(string_field(j, "path"), roots);
    } else if (action == "request") {
      diag().log(diag_level::info, "file.request", std::string(),
                 {{"peer", peer_ip}, {"path", string_field(j, "path")}});
      files_->request(string_field(j, "path"));
      update_file_retry_timer();
    } else if (action == "pull") {
      diag().log(diag_level::info, "file.pull", std::string(),
                 {{"peer", peer_ip}, {"id", std::to_string(id)}});
      files_->pull(id);
      update_file_retry_timer();
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
      // These files came from a viewer. Mark them before touching the
      // clipboard so the watcher cannot announce our own reply back.
      clipboard_mark_files_written(resolved);
      const bool applied =
          options_.clipboard && options_.clipboard->available() &&
          !resolved.empty() && options_.clipboard->set_files(resolved);
      diag().log(applied ? diag_level::info : diag_level::warn,
                 "file.clipboard-set", std::string(),
                 {{"peer", peer_ip},
                  {"ok", applied ? "1" : "0"},
                  {"requested", std::to_string(requested.size())},
                  {"resolved", std::to_string(resolved.size())}});
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
    if (std::getenv("MOLINK_STREAM_LOG") != nullptr) {
      const auto events = j.find("events");
      if (events != j.end() && events->is_array()) {
        for (const auto& event : *events) {
          const std::string kind = string_field(event, "k");
          if (kind != "move") {
            std::fprintf(stderr, "host: input k=%s\n", kind.c_str());
          }
        }
      } else {
        const std::string kind = string_field(j, "k");
        if (kind != "move") {
          std::fprintf(stderr, "host: input k=%s\n", kind.c_str());
        }
      }
    }
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
    // A pressed primary button freezes the tile <-> stream decision: a drag
    // must not pay an encoder restart or a full-screen keyframe, and a release
    // keeps the veto for a short settle window. This reuses the existing input
    // channel; no new event stream is introduced.
    const auto note_pointer = [&](const uvcpp_json& event) {
      if (!options_.capture) return;
      const std::string kind = string_field(event, "k");
      if (kind == "move") {
        options_.capture->note_pointer(false, true, now_ms());
        return;
      }
      if (kind != "button") return;
      double button = 1;
      number_field(event, "button", button);
      if (static_cast<int>(button) != 1) return;
      const bool down = bool_field(event, "down", true);
      if (std::getenv("MOLINK_STREAM_LOG") != nullptr) {
        std::fprintf(stderr, "host: input primary button=%s\n",
                     down ? "down" : "up");
      }
      options_.capture->note_pointer(down, false, now_ms());
    };
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
        note_pointer(event);
      }
    } else if (apply(j)) {
      note_pointer(j);
    }
    if (rejected) {
      diag().log(diag_level::warn, "input.rejected",
                 input_error.empty() ? "input event was rejected"
                                     : input_error,
                 {{"peer", peer_ip}});
      // Injection failures used to leave this empty, so the page could only
      // show the raw `bad_input` code. The injector now names the value, but
      // keep a last-resort message for an older or future backend.
      send_error(transport.get(), "bad_input",
                 input_error.empty() ? "input event was rejected"
                                     : input_error);
    } else if (failed) {
      // SendInput fails silently on the secure desktop and for elevated
      // windows; tell the client instead of dropping every event without a
      // word. Rate-limit it so a moving pointer cannot flood the socket.
      const std::uint64_t now = now_ms();
      if (now - last_input_error_ms_ >= 5000) {
        last_input_error_ms_ = now;
        diag().log(diag_level::warn, "input.unavailable",
                   "the host could not inject input", {{"peer", peer_ip}});
        send_error(transport.get(), "input_unavailable",
                   "the host could not inject input");
      }
    }
    return;
  }

  if (type == "client-log") {
    // The page has no console of its own; the local core's proxy prints what
    // it tunnels here. Accept it quietly -- it used to come back to the client
    // as "unsupported control type".
    if (std::getenv("MOLINK_STREAM_LOG") != nullptr) {
      std::fprintf(stderr, "host: client-log %s\n",
                   string_field(j, "text").c_str());
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
    case protocol::packet_type::clipboard_range: {
      // Reply to a virtual-file `fetch`: u32 request id, u32 error code,
      // u64 offset, then the bytes. An error code makes the waiting Read()
      // fail at once instead of sitting until its timeout.
      if (!session->authenticated) {
        send_error(transport.get(), "unauthorized",
                   "send a hello message with a valid token");
        break;
      }
      if (pkt.payload.size() < 16) {
        send_error(transport.get(), "bad_packet",
                   "clipboard range header is truncated");
        break;
      }
      uint32_t id = 0;
      uint32_t code = 0;
      uint64_t offset = 0;
      std::memcpy(&id, pkt.payload.data(), sizeof(id));
      std::memcpy(&code, pkt.payload.data() + 4, sizeof(code));
      std::memcpy(&offset, pkt.payload.data() + 8, sizeof(offset));
      try {
        if (stream_source_) {
          stream_source_->note_range(id, code, offset,
                                     pkt.payload.data() + 16,
                                     pkt.payload.size() - 16);
        }
      } catch (const std::exception& error) {
        // A malformed/oversized reply must fail the waiting paste, not
        // terminate the event loop.
        diag().log(diag_level::error, "clipboard.pull.error", error.what(),
                   {{"where", "note_range"}});
        try {
          if (stream_source_) {
            stream_source_->note_range(id, 2, offset, nullptr, 0);
          }
        } catch (...) {
        }
      } catch (...) {
        diag().log(diag_level::error, "clipboard.pull.error",
                   "unknown exception", {{"where", "note_range"}});
        try {
          if (stream_source_) {
            stream_source_->note_range(id, 2, offset, nullptr, 0);
          }
        } catch (...) {
        }
      }
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
  {
    const uint64_t now = now_ms();
    const uint64_t duration =
        session->connected_ms != 0 && now > session->connected_ms
            ? now - session->connected_ms
            : 0;
    std::vector<diag_field> fields;
    fields.push_back({"peer", session->peer_ip});
    fields.push_back({"session_id", session->public_id});
    fields.push_back({"close_code", std::to_string(code)});
    if (!reason.empty()) fields.push_back({"reason", reason});
    fields.push_back({"ms", std::to_string(duration)});
    fields.push_back({"auth", session->authenticated ? "1" : "0"});
    diag().log(diag_level::info, "session.close", std::string(), fields);
  }
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
  if (file_retry_timer_) file_retry_timer_->stop();
  // Wake any clipboard thread blocked on this session's lazy offer before the
  // transport goes away, then release the host clipboard if we owned it.
  stream_source_.reset();
  lazy_source_.reset();
  if (options_.lazy && options_.lazy->clear_session(session->id) &&
      options_.clipboard) {
    options_.clipboard->uninstall_lazy();
  }
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
  // Push what the host user copies to every viewer, the way RDP redirects the
  // clipboard in both directions. The page already pushes its own clipboard
  // here (clipboard-sync), so this closes the return path: text copied inside
  // the remote desktop -- by an application, or by an RDP client with its own
  // clipboard redirection -- reaches the viewer's clipboard without a click.
  if (options_.clipboard) {
    const std::shared_ptr<session_registry> registry = registry_;
    const auto post = options_.post;
    const std::string device = options_.device_id;
    options_.clipboard->watch([registry, post, device](std::string text) {
      if (text.empty() || text.size() > 192 * 1024) return;
      // The clipboard is text now; a later copy of the same file list is a
      // new action and must be announced again.
      clipboard_reset_files_signature();
      const std::string hash = clipboard_hash(text);
      if (!clipboard_take_hash(hash)) return;
      auto deliver = [registry, text = std::move(text), hash, device] {
        std::size_t sent = 0;
        for (const session_info& peer : registry->snapshot()) {
          if (!peer.authenticated || !peer.transport) continue;
          ++sent;
          uvcpp_json out;
          out["type"] = "clipboard-sync";
          out["text"] = text;
          out["hash"] = hash;
          out["chain"] = device;
          send_json(peer.transport.get(), out);
        }
        // Bytes and viewer count only: never the text itself.
        diag().log(diag_level::info, "clipboard.announce",
                   "host text copied",
                   {{"bytes", std::to_string(text.size())},
                    {"viewers", std::to_string(sent)}});
      };
      if (post) {
        post(deliver);
      } else {
        deliver();
      }
    });
    // Files copied on the host (Explorer Ctrl+C) are announced to every viewer
    // as session-scoped ids, plus the legacy path list for the files panel.
    // Publish only on a real change; the signature table also stops a list
    // this machine just materialised for a viewer from echoing back.
    const std::shared_ptr<clipboard_service> clipboard = options_.clipboard;
    options_.clipboard->watch_files(
        [registry, post, clipboard](std::vector<clipboard_file> files) {
          if (files.empty()) return;
          publish_clipboard_files(registry, post, std::move(files),
                                  clipboard && clipboard->nested_rdp());
        });
  }
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
    // The requested rate, for tiles and the stream alike: a viewer that
    // cannot keep up lowers it through note_viewer() below.
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
      bool all_stream = true;
      bool all_h264_stream = true;
      bool all_vp9_stream = true;
      bool all_vp8_stream = true;
      /// A live viewer asked for a preset that must not be chroma-subsampled
      /// (高清/真彩色). The low-delay 4:2:0 stream is then kept off so the BGRA
      /// tile path is what actually reaches that viewer.
      bool no_subsampling = false;
      int viewers = 0;
      // A tab that is gone but left its socket half-open keeps its hello
      // capabilities in the registry for minutes, and one such session with a
      // stale codec list vetoed the codec every live viewer could take. A
      // viewer that sent nothing (no pong, no frame request, no stats) for
      // this long no longer counts.
      constexpr uint64_t kViewerIdleMs = 30000;
      const uint64_t seen_now = now_ms();
      const auto still_watching = [seen_now](const session_info& peer) {
        const uint64_t last = peer.last_seen_ms != 0 ? peer.last_seen_ms
                                                     : peer.connected_ms;
        return seen_now >= last && seen_now - last <= kViewerIdleMs;
      };
      for (const session_info& peer : registry_->snapshot()) {
        if (!peer.authenticated) continue;
        if (!still_watching(peer)) continue;
        if (peer.no_subsampling) no_subsampling = true;
        // A session that neither receives the push clock nor advertised any
        // video decoder is a control/chat/file client, not a viewer. Counting
        // it in the "all viewers can decode X" test made a single such
        // session disable the stream for every real viewer.
        const bool video_viewer =
            peer.video_push || peer.av1_stream || peer.h264_stream ||
            peer.vp9_stream || peer.vp8_stream;
        if (!video_viewer) continue;
        live = true;
        ++viewers;
        if (!peer.av1_stream) all_stream = false;
        if (!peer.h264_stream) all_h264_stream = false;
        if (!peer.vp9_stream) all_vp9_stream = false;
        if (!peer.vp8_stream) all_vp8_stream = false;
      }
      // Weight-based QoS: the slowest live viewer decides. Its painted frame
      // rate, dropped frames and measured RTT move the encoder around the
      // toolbar's anchors; a still screen (no viewers producing frames) never
      // moves anything.
      {
        double slowest_fps = 0.0;
        int worst_dropped = 0;
        double worst_rtt = 0.0;
        for (const session_info& peer : registry_->snapshot()) {
          if (!peer.authenticated || !still_watching(peer)) continue;
          if (peer.viewer_stats_ms == 0 ||
              now_ms() - peer.viewer_stats_ms > 3000) {
            continue;
          }
          if (peer.viewer_paint_fps > 0) {
            slowest_fps = slowest_fps <= 0.0
                              ? peer.viewer_paint_fps
                              : (std::min)(slowest_fps, peer.viewer_paint_fps);
          }
          worst_dropped = (std::max)(worst_dropped, peer.viewer_dropped);
          worst_rtt = (std::max)(worst_rtt, peer.viewer_rtt_ms);
        }
        stream_qos::input qin;
        qin.paint_fps = slowest_fps;
        qin.target_fps = options_.capture->target_fps();
        qin.dropped = worst_dropped;
        qin.rtt_ms = worst_rtt;
        qin.demand = viewers > 0;
        if (qos_.note(qin, seen_now)) {
          const stream_qos::output q = qos_.state();
          options_.capture->set_stream_qos(q.bitrate_scale, q.qp_offset,
                                           q.force_cbr);
        }
      }
      // The host settings page can switch AV1 off entirely; it still only
      // runs when every watching client can decode it.
      const bool enabled =
          !options_.auth || options_.auth->settings().av1_enabled;
      // The host's global codec preference is read before the admission
      // tests: an explicitly pinned encoder changes them (no tile fallback,
      // and 4:2:0 wins over a truecolour request).
      const std::string preference =
          options_.auth ? options_.auth->settings().stream_codec : "auto";
      const bool explicit_codec = is_explicit_stream_codec(preference);
      // Every stream encoder is 4:2:0. A user who pinned VP9 gets the stream
      // at 4:2:0 even if the viewer's active tier asked for 4:4:4; the UI
      // says so instead of silently leaving the stream off.
      const bool no_subsampling_effective =
          effective_no_subsampling(no_subsampling, preference);
      // Single-frame AV1 is retired, so the hello's `av1` flag is gone and the
      // permission now follows the low-delay stream capability. Without this
      // the pinned/laddered AV1 was always refused and silently fell back to
      // H.264.
      const bool allow = enabled && live && all_stream && !no_subsampling_effective;
      // What this host can actually encode. The viewer set only says what is
      // decodable; a Windows build without libvpx still has NVENC H.264, and
      // choosing VP9 merely because every viewer can decode it made the
      // capture fail to create an encoder and stay on tiles forever. Keep the
      // viewer and host capability sets intersected.
      bool host_av1 = false;
      bool host_h264 = false;
      bool host_vp9 = false;
      bool host_vp8 = false;
      bool av1_hardware = false;
      for (const video_encoder_backend& backend : video_encoder_backends()) {
        if (!backend.available) continue;
        host_av1 = host_av1 || backend.av1_stream;
        host_h264 = host_h264 || backend.h264_stream;
        host_vp9 = host_vp9 || backend.vp9_stream;
        host_vp8 = host_vp8 || backend.vp8_stream;
        if (backend.hardware && backend.av1_stream) av1_hardware = true;
      }
      // A pinned codec wins when every viewer can decode it; the automatic
      // hardware-first order applies otherwise. An explicit pin is never
      // replaced by another rung: if it is unavailable the picture stays on
      // tiles and the viewer is told which encoder is missing.
      stream_choice pinned = stream_choice::none;
      if (preference == "av1" && allow && all_stream && host_av1) {
        pinned = stream_choice::av1;
      } else if (preference == "h264" && all_h264_stream && host_h264) {
        pinned = stream_choice::h264;
      } else if (preference == "vp9" && all_vp9_stream && host_vp9) {
        pinned = stream_choice::vp9;
      } else if (preference == "vp8" && all_vp8_stream && host_vp8) {
        pinned = stream_choice::vp8;
      }
      const bool codec_available =
          !explicit_codec || pinned != stream_choice::none;
      // A stream round is only worth starting when at least one viewer can
      // actually decode it. With a tiles-only viewer (or a host build without
      // the encoders) the join window below used to hold the stream path for
      // kJoinStreamMs and the first picture waited for it; tiles can start at
      // once instead. An explicit codec uses the pinned result: a missing
      // encoder is reported, not replaced by a different one.
      const bool can_stream =
          explicit_codec
              ? (pinned != stream_choice::none)
              : (!no_subsampling_effective &&
                 ((allow && host_av1) || (all_h264_stream && host_h264) ||
                  (all_vp9_stream && host_vp9) || (all_vp8_stream && host_vp8)));
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
      const std::string video_mode =
          options_.auth ? options_.auth->settings().video_mode : "tiles";
      // The secure desktop cannot use tiles: it is captured through the
      // helper at a few frames per second, so a pinned "tiles" is ignored
      // there and the stream is always forced.
      const bool secure_desktop =
          options_.capture && options_.capture->is_secure_desktop();
      if (options_.capture) {
        // "stream" forces the stream unless the viewer picked tiles by hand;
        // "auto" and "tiles" release it, and the capture's stream_policy
        // decides from the movement average. Re-read every tick so a toolbar
        // choice takes effect without reconnecting.
        const int enter_percent =
            options_.auth ? options_.auth->settings().stream_enter_percent : 15;
        // `codec_available` keeps the lock off while the pinned encoder
        // cannot run on this host; the viewer gets an explicit
        // "codec unavailable" status instead of a frozen stream.
        apply_video_mode(*options_.capture, video_mode, preference,
                         enter_percent, secure_desktop, codec_available);
      }
      // An explicit encoder is a lock: it also overrides a "tiles" video
      // mode. Only the explicit "tiles" preference itself means tiles.
      const bool stream_allowed =
          secure_desktop || explicit_codec ||
          (video_mode != "tiles" && preference != "tiles");
      // While the secure desktop is in front, keep renewing the forced-stream
      // window: a one-shot request expired after ten seconds and the static
      // logon screen fell back to tiles in the middle of a login.
      if (secure_desktop && options_.capture) {
        options_.capture->request_stream(2500);
      }
      // A session that just joined starts on the stream, not on tiles: the
      // first tile keyframe of a full screen is the expensive one and made
      // the first seconds stutter. The movement rule takes over after this
      // window, so a still screen still settles on tiles ten seconds in.
      if (live && !had_viewers_ && stream_allowed && can_stream &&
          options_.capture) {
        options_.capture->request_stream(kJoinStreamMs);
        had_viewers_ = true;
      } else if (!live) {
        had_viewers_ = false;
      }
      // CPU SVT-AV1 at a few frames per second is worse than any other
      // stream, so "auto" only ranks AV1 first when this host has a hardware
      // AV1 encoder (NVENC). Pinning AV1 by hand still selects it.
      // An explicit codec is never substituted: if it is unavailable the
      // result is `none` (tiles) and the viewer shows the error, rather than
      // silently switching to another encoder.
      stream_choice stream = stream_choice::none;
      if (live && viewers > 0 && stream_allowed && codec_available) {
        if (explicit_codec) {
          stream = pinned;
        } else if (!no_subsampling_effective) {
          stream = pinned != stream_choice::none
                       ? pinned
                       : choose_video_codec(
                             allow && host_av1, all_stream,
                             all_h264_stream && host_h264,
                             all_vp9_stream && host_vp9,
                             all_vp8_stream && host_vp8, av1_hardware);
        }
      }
      options_.capture->set_av1_stream_allowed(stream == stream_choice::av1);
      options_.capture->set_h264_stream_allowed(stream == stream_choice::h264);
      // Tell viewers when an explicit codec cannot run here. The status is
      // sent on change only; `available` false keeps the picture on tiles but
      // the UI names the encoder instead of pretending it is running.
      {
        static std::string reported_codec;
        static bool reported_available = true;
        if (preference != reported_codec ||
            codec_available != reported_available) {
          reported_codec = preference;
          reported_available = codec_available;
          uvcpp_json status;
          status["type"] = "codec-status";
          status["codec"] = preference;
          status["available"] = codec_available;
          for (const session_info& peer : registry_->snapshot()) {
            if (!peer.authenticated || !peer.transport) continue;
            send_json(peer.transport.get(), status);
          }
        }
      }
      if (std::getenv("MOLINK_STREAM_LOG") != nullptr) {
        static int last_stream = -1;
        if (static_cast<int>(stream) != last_stream) {
          last_stream = static_cast<int>(stream);
          std::fprintf(stderr,
                       "host: stream choice=%d (all av1=%d h264=%d vp9=%d "
                       "vp8=%d secure=%d)\n",
                       static_cast<int>(stream), all_stream ? 1 : 0,
                       all_h264_stream ? 1 : 0, all_vp9_stream ? 1 : 0,
                       all_vp8_stream ? 1 : 0, secure_desktop ? 1 : 0);
          for (const session_info& peer : registry_->snapshot()) {
            if (!peer.authenticated) continue;
            std::fprintf(stderr,
                         "host: viewer %u av1_stream=%d h264=%d vp9=%d vp8=%d "
                         "push=%d image=%d transport=%d idle_ms=%llu\n",
                         peer.id, peer.av1_stream ? 1 : 0,
                         peer.h264_stream ? 1 : 0, peer.vp9_stream ? 1 : 0,
                         peer.vp8_stream ? 1 : 0, peer.video_push ? 1 : 0,
                         peer.has_image ? 1 : 0, peer.transport ? 1 : 0,
                         static_cast<unsigned long long>(
                             now_ms() - (peer.last_seen_ms != 0
                                             ? peer.last_seen_ms
                                             : peer.connected_ms)));
          }
        }
        // What the viewer's own pipeline achieved, once a second: a page that
        // paints far below the wire rate is the limit, not the encoder.
        for (const session_info& peer : registry_->snapshot()) {
          if (!peer.authenticated) continue;
          std::fprintf(stderr,
                       "host: stats viewer %u paint=%.1f dropped=%d rx=%.0f "
                       "image=%d seq=%lld\n",
                       peer.id, peer.viewer_paint_fps, peer.viewer_dropped,
                       peer.viewer_rx_rate, peer.has_image ? 1 : 0,
                       static_cast<long long>(peer.last_capture_seq));
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
            clipboard_reset_files_signature();
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
      const std::shared_ptr<clipboard_service> clipboard = options_.clipboard;
      options_.clipboard->request_files(
          [registry_ref, post, clipboard](std::vector<clipboard_file> files) {
            g_clipboard_sync.files_pending.store(false);
            if (files.empty()) return;
            publish_clipboard_files(registry_ref, post, std::move(files),
                                    clipboard && clipboard->nested_rdp());
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
    // A snapshot timestamp can be newer than `now`: a message that arrived
    // while this tick was being assembled updates last_seen_ms past the value
    // read at the top of the loop. Unsigned subtraction then wraps to ~2^64
    // and every "older than N ms" test fires. That is what closed healthy
    // sessions with a bogus `1001: idle timeout` every few seconds.
    const auto elapsed_since = [now](uint64_t then) -> uint64_t {
      return now > then ? now - then : 0;
    };
    for (const session_info& s : sessions) {
      if (!s.authenticated) {
        // A pending approval is a deliberate wait, not a stuck client.
        const uint64_t limit = s.awaiting_approval
                                   ? 120000
                                   : static_cast<uint64_t>(
                                         options_.hello_timeout_ms);
        if (elapsed_since(s.connected_ms) > limit) {
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

      if (elapsed_since(s.last_ping_ms) >=
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
          elapsed_since(s.last_seen_ms) >
              static_cast<uint64_t>(options_.idle_close_ms)) {
        if (std::getenv("MOLINK_STREAM_LOG") != nullptr) {
          std::fprintf(stderr,
                       "host: idle close id=%u now=%llu last_seen=%llu "
                       "idle_ms=%d\n",
                       s.id, static_cast<unsigned long long>(now),
                       static_cast<unsigned long long>(s.last_seen_ms),
                       options_.idle_close_ms);
        }
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
