#include "ws_session.h"

#include <chrono>
#include <cstdint>
#include <vector>

#include <web/uvcpp_ws_connection.h>
#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_json.h>

#include "config.h"
#include "protocol/frame_packet.h"
#include "util.h"
#include "version.h"

namespace molink {
namespace {

using uvcpp::uvcpp_json;
using uvcpp::uvcpp_json_dump;
using uvcpp::uvcpp_ws_connection;
using uvcpp::ws_close_code;

uint64_t epoch_us() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<microseconds>(system_clock::now().time_since_epoch())
          .count());
}

/// Replaying more than this means the client is hopelessly behind; a keyframe
/// is cheaper than shipping dozens of deltas it will render in one go.
constexpr uint32_t kMaxReplayFrames = 60;

std::string string_field(const uvcpp_json& j, const char* key) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_string()) return std::string();
  return it->get<std::string>();
}

void send_json(uvcpp_ws_connection* conn, const uvcpp_json& j) {
  if (conn == nullptr || !conn->is_open()) return;
  const std::string body = uvcpp_json_dump(j);
  conn->send_text(body.data(), body.size());
}

void send_error(uvcpp_ws_connection* conn, const char* code,
                const std::string& message) {
  uvcpp_json j;
  j["type"] = "error";
  j["code"] = code;
  j["message"] = message;
  send_json(conn, j);
}

void send_pong(uvcpp_ws_connection* conn, const uvcpp_json& request) {
  uvcpp_json j;
  j["type"] = "pong";
  const auto it = request.find("t");
  if (it != request.end() && it->is_number()) j["t"] = *it;
  j["server_ms"] = now_ms();
  send_json(conn, j);
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

  err = "unknown input event kind: " + kind;
  return input_injector::status::rejected;
}

bool send_frame_payload(uvcpp_ws_connection* conn,
                        const std::shared_ptr<session_info>& session,
                        uint16_t flags, uint64_t ts_us,
                        const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> bytes;
  std::string err;
  const uint32_t seq = session->out_seq++;
  if (!protocol::encode_packet_into(
          bytes, static_cast<uint8_t>(protocol::packet_type::frame), flags,
          session->id, seq, ts_us, payload.data(), payload.size(), err)) {
    send_error(conn, "frame_build_failed", err);
    return false;
  }
  if (conn->send_binary(reinterpret_cast<const char*>(bytes.data()),
                        bytes.size()) != 0) {
    send_error(conn, "send_failed", "could not queue frame");
    return false;
  }
  ++session->frames_sent;
  return true;
}

/// A frame with no tiles. It still advances the wire sequence, which tells the
/// client "nothing changed" without leaving its request in flight forever.
void send_empty_frame(uvcpp_ws_connection* conn,
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
    send_error(conn, "frame_build_failed", err);
    return;
  }
  send_frame_payload(conn, session,
                     keyframe ? protocol::kPacketFlagKeyframe : 0, epoch_us(),
                     payload);
}

/// Cursor bitmap and position are sent alongside frames: they are small, change
/// independently of screen damage, and are not part of an X11 grab.
void maybe_send_cursor(uvcpp_ws_connection* conn,
                       const std::shared_ptr<session_info>& session,
                       const ws_options& opt) {
  if (!opt.capture) return;
  cursor_snapshot cursor;
  if (!opt.capture->cursor(cursor)) return;

  const bool visible = cursor.width > 0 && cursor.height > 0;
  if (!visible) {
    if (session->cursor_visible) {
      uvcpp_json j;
      j["type"] = "cursor-position";
      j["visible"] = false;
      send_json(conn, j);
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
    send_json(conn, j);
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
    send_json(conn, j);
    session->cursor_x = cursor.x;
    session->cursor_y = cursor.y;
    session->cursor_visible = true;
  }
}

void send_capture_frame(uvcpp_ws_connection* conn,
                        const std::shared_ptr<session_info>& session,
                        const ws_options& opt) {
  if (!opt.capture) {
    send_empty_frame(conn, session, 0, 0, false);
    return;
  }

  const std::shared_ptr<const captured_frame> latest = opt.capture->latest();
  if (!latest) {
    // Capture has not produced anything yet; let the client keep polling.
    send_empty_frame(conn, session, 0, 0, false);
    return;
  }

  if (!session->has_image) {
    if (latest->keyframe) {
      session->has_image = true;
      session->last_capture_seq = latest->seq;
      send_frame_payload(conn, session, protocol::kPacketFlagKeyframe,
                         latest->ts_us, latest->payload);
      return;
    }
    opt.capture->request_keyframe();
    send_empty_frame(conn, session, latest->width, latest->height, false);
    return;
  }

  const int64_t last = session->last_capture_seq;
  if (last < 0 || static_cast<uint32_t>(last) >= latest->seq) {
    // Nothing new since the last response. Answer anyway so the client's
    // request does not stay in flight.
    send_empty_frame(conn, session, latest->width, latest->height, false);
    return;
  }

  // Replay every frame the session has not seen. Capture keeps a bounded
  // history precisely so a poll that arrived late does not need a keyframe.
  const uint32_t missing = latest->seq - static_cast<uint32_t>(last);
  if (missing > kMaxReplayFrames) {
    session->has_image = false;
    session->last_capture_seq = -1;
    opt.capture->request_keyframe();
    send_empty_frame(conn, session, latest->width, latest->height, false);
    return;
  }

  for (uint32_t seq = static_cast<uint32_t>(last) + 1; seq <= latest->seq;
       ++seq) {
    const std::shared_ptr<const captured_frame> frame = opt.capture->find(seq);
    if (!frame) {
      // Evicted from history. Resynchronize from a fresh keyframe.
      session->has_image = false;
      session->last_capture_seq = -1;
      opt.capture->request_keyframe();
      send_empty_frame(conn, session, latest->width, latest->height, false);
      return;
    }
    session->last_capture_seq = seq;
    if (frame->keyframe) session->has_image = true;
    send_frame_payload(conn, session,
                       frame->keyframe ? protocol::kPacketFlagKeyframe : 0,
                       frame->ts_us, frame->payload);
  }
}

void send_welcome(uvcpp_ws_connection* conn, const session_info& session,
                  const ws_options& opt) {
  const std::shared_ptr<const captured_frame> frame =
      opt.capture ? opt.capture->latest() : nullptr;

  uvcpp_json j;
  j["type"] = "welcome";
  j["session"] = session.public_id;
  j["session_id"] = session.id;
  j["protocol"] = protocol::kProtocolVersion;
  j["service"] = MOLINK_CORE_SERVICE;
  j["version"] = opt.version;
  j["transport"] = "websocket";
  j["tiles"]["codec"] =
      (frame && frame->codec == static_cast<uint8_t>(protocol::codec::deflate_bgra))
          ? "deflate-bgra"
          : "bgra";
  j["tiles"]["screen_w"] = frame ? frame->width : 0;
  j["tiles"]["screen_h"] = frame ? frame->height : 0;
  j["tiles"]["tile_w"] = frame ? frame->tile_w : 0;
  j["tiles"]["tile_h"] = frame ? frame->tile_h : 0;
  j["tiles"]["backend"] = opt.capture ? opt.capture->backend() : "none";
  j["tiles"]["synthetic"] = opt.capture ? !opt.capture->is_real() : true;
  j["input"] = opt.input != nullptr && opt.input->available();
  j["input_backend"] = opt.input ? opt.input->backend() : "none";
  j["clipboard"] = opt.clipboard != nullptr && opt.clipboard->available();
  j["clipboard_backend"] = opt.clipboard ? opt.clipboard->backend() : "none";
  j["device_id"] = opt.device_id;
  send_json(conn, j);
}

}  // namespace

uvcpp::uvcpp_web_ws_handler make_ws_handler(
    std::shared_ptr<session_registry> registry, ws_options options) {
  return [registry, options](uvcpp::uvcpp_web_ws_request& ws) {
    uvcpp_ws_connection* conn = ws.connection();
    if (conn == nullptr) return;

    // Control messages are small JSON documents; the bulk data travels in the
    // other direction as binary packets.
    conn->set_max_message_size(1024u * 1024u);

    const std::shared_ptr<session_info> session =
        registry->add(conn, ws.request().peer_ip(), ws.request().peer_port());

    if (const std::string* token = ws.query("token")) {
      // A locked core leaves the session unauthenticated; the hello handler
      // reports "locked" once the connection is fully established, where the
      // error and close frame are actually delivered.
      if (constant_time_equals(*token, options.token) &&
          !(options.lock && options.lock->locked())) {
        session->authenticated = true;
      }
    }
    if (session->authenticated && options.capture) {
      options.capture->set_active(true);
    }

    if (session->authenticated) {
      send_welcome(conn, *session, options);
    } else {
      uvcpp_json j;
      j["type"] = "hello_required";
      j["protocol"] = protocol::kProtocolVersion;
      j["service"] = MOLINK_CORE_SERVICE;
      j["version"] = options.version;
      send_json(conn, j);
    }

    auto require_auth = [conn, session, options]() -> bool {
      if (session->authenticated) return true;
      send_error(conn, "unauthorized", "send a hello message with a valid token");
      return false;
    };

    conn->on_text([conn, session, registry, options,
                   require_auth](const std::string& text) {
      session->last_seen_ms = now_ms();
      if (options.lock) options.lock->touch();

      uvcpp_json j;
      const uvcpp::json_status status = uvcpp::uvcpp_json_parse(text, j);
      if (status != uvcpp::json_status::OK || !j.is_object()) {
        send_error(conn, "bad_json", "control message must be a JSON object");
        return;
      }
      const std::string type = string_field(j, "type");

      if (type == "hello") {
        if (!constant_time_equals(string_field(j, "token"), options.token)) {
          send_error(conn, "unauthorized", "invalid token");
          conn->close(ws_close_code::POLICY_VIOLATION, "invalid token");
          return;
        }
        if (options.lock && options.lock->locked()) {
          send_error(conn, "locked", "the UI is locked");
          conn->close(ws_close_code::POLICY_VIOLATION, "locked");
          return;
        }
        session->authenticated = true;
        if (options.capture) options.capture->set_active(true);
        send_welcome(conn, *session, options);
        return;
      }

      if (type == "ping") {
        send_pong(conn, j);
        return;
      }

      if (!require_auth()) {
        conn->close(ws_close_code::POLICY_VIOLATION, "not authenticated");
        return;
      }

      if (type == "clipboard") {
        if (!options.clipboard || !options.clipboard->available()) {
          send_error(conn, "not_implemented", "clipboard is unavailable on this host");
          return;
        }
        const std::string action = string_field(j, "action");
        if (action == "set") {
          std::string text = string_field(j, "text");
          constexpr std::size_t kMaxText = 192 * 1024;
          if (text.size() > kMaxText) {
            send_error(conn, "clipboard_too_large",
                       "clipboard text exceeds 192 KiB");
            return;
          }
          options.clipboard->set_text(std::move(text));
          return;
        }
        if (action == "get") {
          auto registry_ref = registry;
          auto post = options.post;
          const uint32_t session_id = session->id;
          options.clipboard->request_text(
              [registry_ref, post, session_id](std::string text) {
                if (!post) return;
                post([registry_ref, session_id, text = std::move(text)] {
                  const auto live = registry_ref->find(session_id);
                  if (!live || !live->conn) return;
                  uvcpp_json j;
                  j["type"] = "clipboard";
                  j["text"] = text;
                  send_json(live->conn, j);
                });
              });
          return;
        }
        send_error(conn, "bad_clipboard", "action must be set or get");
        return;
      }

      if (type == "request-frame") {
        maybe_send_cursor(conn, session, options);
        send_capture_frame(conn, session, options);
        return;
      }

      if (type == "request-keyframe") {
        if (options.capture) options.capture->request_keyframe();
        return;
      }

      if (type == "bye") {
        conn->close(ws_close_code::NORMAL, "client said bye");
        return;
      }

      if (type == "input") {
        if (!options.input || !options.input->available()) {
          uvcpp_json e;
          e["type"] = "error";
          e["code"] = "not_implemented";
          e["what"] = "input";
          e["message"] = "input injection is unavailable on this host";
          send_json(conn, e);
          return;
        }

        std::string input_error;
        bool rejected = false;
        const auto events = j.find("events");
        if (events != j.end() && events->is_array()) {
          if (events->size() > 64) {
            send_error(conn, "bad_input", "at most 64 events per message");
            return;
          }
          for (const auto& event : *events) {
            if (inject_one(*options.input, event, input_error) ==
                input_injector::status::rejected) {
              rejected = true;
              break;
            }
          }
        } else if (inject_one(*options.input, j, input_error) ==
                   input_injector::status::rejected) {
          rejected = true;
        }
        if (rejected) send_error(conn, "bad_input", input_error);
        return;
      }

      send_error(conn, "unknown_type", "unsupported control type: " + type);
    });

    conn->on_binary([conn, session, options](const uint8_t* data, std::size_t len) {
      session->last_seen_ms = now_ms();
      if (options.lock) options.lock->touch();
      protocol::packet pkt;
      std::string err;
      if (!protocol::decode_packet(data, len, pkt, err)) {
        send_error(conn, "bad_packet", err);
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
            conn->send_binary(reinterpret_cast<const char*>(out.data()),
                              out.size());
          }
          break;
        }
        case protocol::packet_type::input:
          send_error(conn, "not_implemented", "input injection is not implemented");
          break;
        default:
          send_error(conn, "unexpected_packet", "packet type not accepted here");
          break;
      }
    });

    conn->on_close([registry, session, options](ws_close_code code,
                                                const std::string& reason) {
      registry->remove(session->id);
      // Stop grabbing the screen once nobody is watching.
      if (options.capture && !registry->has_authenticated()) {
        options.capture->set_active(false);
      }
      // A dropped connection must not leave a button or modifier held down on
      // the remote machine.
      if (options.input && !registry->has_authenticated()) {
        options.input->release_all();
      }
      (void)code;
      (void)reason;
    });
  };
}

session_supervisor::session_supervisor(
    uvcpp::uvcpp_web_app& app, std::shared_ptr<session_registry> registry,
    ws_options options)
    : app_(app), registry_(std::move(registry)), options_(std::move(options)) {}

session_supervisor::~session_supervisor() { stop(); }

void session_supervisor::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { run(); });
}

void session_supervisor::stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
}

void session_supervisor::run() {
  using namespace std::chrono;
  const auto tick = milliseconds(250);
  while (running_.load()) {
    for (int i = 0; i < 4 && running_.load(); ++i) {
      std::this_thread::sleep_for(tick);
    }
    if (!running_.load()) break;

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
    for (const session_info& s : sessions) {
      if (!s.authenticated) {
        if (now - s.connected_ms >
            static_cast<uint64_t>(options_.hello_timeout_ms)) {
          const uint32_t id = s.id;
          app_.post([registry = registry_, id] {
            const auto live = registry->find(id);
            if (live && live->conn) {
              live->conn->close(ws_close_code::POLICY_VIOLATION,
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
          if (!live || !live->conn || !live->authenticated) return;
          uvcpp_json j;
          j["type"] = "ping";
          j["t"] = now_ms();
          send_json(live->conn, j);
          live->last_ping_ms = now_ms();
        });
      }

      if (now - s.last_seen_ms >
          static_cast<uint64_t>(options_.idle_close_ms)) {
        const uint32_t id = s.id;
        app_.post([registry = registry_, id] {
          const auto live = registry->find(id);
          if (live && live->conn) {
            live->conn->close(ws_close_code::GOING_AWAY, "idle timeout");
          }
        });
      }
    }
  }
}

}  // namespace molink
