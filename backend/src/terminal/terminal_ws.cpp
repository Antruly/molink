#include "terminal/terminal_ws.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <webapp/uvcpp_web_json.h>

#include "auth_state.h"
#include "diag_log.h"

namespace molink {
namespace {

// Binary frame layout. Both directions start with a kind byte so the channel
// can keep carrying raw bytes instead of base64 in JSON.
//
//   output (host -> viewer): [0x01][u64 id][u64 seq][payload]
//   input  (viewer -> host): [0x02][u64 id][payload]
constexpr uint8_t kFrameOutput = 0x01;
constexpr uint8_t kFrameInput = 0x02;
constexpr std::size_t kOutputHeaderSize = 1 + 8 + 8;
constexpr std::size_t kInputHeaderSize = 1 + 8;
constexpr std::size_t kReplayChunk = 64u * 1024u;

void put_u64(uint8_t* out, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xff);
  }
}

uint64_t get_u64(const uint8_t* in) {
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<uint64_t>(in[i]) << (8 * i);
  }
  return value;
}

class terminal_connection final
    : public std::enable_shared_from_this<terminal_connection> {
 public:
  terminal_connection(uvcpp::uvcpp_ws_connection* conn,
                      std::shared_ptr<terminal_service> service,
                      uint64_t viewer)
      : conn_(conn), service_(std::move(service)), viewer_(viewer) {}

  ~terminal_connection() { cleanup(); }

  void start() {
    if (conn_ == nullptr) return;
    // Control messages are small; terminal input is sent in ordinary binary
    // frames, so a 1 MiB cap leaves plenty of room for a paste.
    conn_->set_max_message_size(1024u * 1024u);
    std::shared_ptr<terminal_connection> self = shared_from_this();
    conn_->on_text([self](const std::string& text) { self->on_text(text); });
    conn_->on_binary([self](const uint8_t* data, std::size_t len) {
      self->on_binary(data, len);
    });
    conn_->on_close([self](uvcpp::ws_close_code, const std::string&) {
      self->on_close();
    });

    std::weak_ptr<terminal_connection> weak = weak_from_this();
    listener_ = service_->add_listener(
        [weak](uint64_t id, uint64_t seq, const uint8_t* data,
               std::size_t len) {
          if (auto self = weak.lock()) self->on_output(id, seq, data, len);
        },
        [weak](uint64_t id, int code) {
          if (auto self = weak.lock()) self->on_exit(id, code);
        });
  }

 private:
  void cleanup() {
    if (listener_ != 0) {
      service_->remove_listener(listener_);
      listener_ = 0;
    }
    if (viewer_ != 0) {
      service_->release_viewer(viewer_);
      viewer_ = 0;
    }
    conn_ = nullptr;
    closed_ = true;
  }

  void on_close() { cleanup(); }

  void send_json(const uvcpp::uvcpp_json& json) {
    if (closed_ || conn_ == nullptr) return;
    const std::string body = uvcpp::uvcpp_json_dump(json);
    conn_->send_text(body.data(), body.size());
  }

  /// `id` names the terminal the error belongs to, when there is one. The
  /// page uses it to flag that tab instead of showing a detached error.
  void send_error(const char* code, const std::string& message,
                  uint64_t id = 0) {
    uvcpp::uvcpp_json json;
    json["type"] = "error";
    json["code"] = code;
    if (!message.empty()) json["message"] = message;
    if (id != 0) json["id"] = id;
    send_json(json);
  }

  void send_output(uint64_t id, uint64_t seq, const uint8_t* data,
                   std::size_t len) {
    if (closed_ || conn_ == nullptr || len == 0) return;
    std::vector<uint8_t> frame(kOutputHeaderSize + len);
    frame[0] = kFrameOutput;
    put_u64(frame.data() + 1, id);
    put_u64(frame.data() + 9, seq);
    std::memcpy(frame.data() + kOutputHeaderSize, data, len);
    conn_->send_binary(reinterpret_cast<const char*>(frame.data()),
                       frame.size());
  }

  void send_replay(uint64_t id, uint64_t seq,
                   const std::vector<uint8_t>& data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
      // Parenthesised: windows.h defines min()/max() as macros, and MSVC then
      // rejects `std::min` with C2589.
      const std::size_t chunk = (std::min)(kReplayChunk, data.size() - offset);
      send_output(id, seq + offset, data.data() + offset, chunk);
      offset += chunk;
    }
  }

  void on_output(uint64_t id, uint64_t seq, const uint8_t* data,
                 std::size_t len) {
    if (attached_.count(id) == 0) return;
    send_output(id, seq, data, len);
  }

  void on_exit(uint64_t id, int code) {
    if (attached_.erase(id) == 0) return;
    uvcpp::uvcpp_json json;
    json["type"] = "term-exit";
    json["id"] = id;
    json["code"] = code;
    send_json(json);
  }

  void on_binary(const uint8_t* data, std::size_t len) {
    if (closed_ || data == nullptr) return;
    if (len < kInputHeaderSize || data[0] != kFrameInput) return;
    const uint64_t id = get_u64(data + 1);
    if (attached_.count(id) == 0) return;
    service_->write(id, data + kInputHeaderSize, len - kInputHeaderSize);
  }

  void on_text(const std::string& text) {
    if (closed_) return;
    uvcpp::uvcpp_json request;
    if (uvcpp::uvcpp_json_parse(text, request) != uvcpp::json_status::OK ||
        !request.is_object()) {
      send_error("bad_terminal_request", "expected a JSON object");
      return;
    }
    const std::string type =
        uvcpp::uvcpp_json_get_string(request, "type", std::string());
    if (type == "term-open") {
      handle_open(request);
    } else if (type == "term-attach") {
      handle_attach(request);
    } else if (type == "term-resize") {
      handle_resize(request);
    } else if (type == "term-close") {
      handle_close(request);
    } else {
      send_error("bad_terminal_request", "unknown message type");
    }
  }

  bool read_id(const uvcpp::uvcpp_json& request, uint64_t& id) {
    long long value = 0;
    if (!uvcpp::uvcpp_json_get_int(request, "id", value) || value <= 0) {
      send_error("bad_terminal_request", "a positive id is required");
      return false;
    }
    id = static_cast<uint64_t>(value);
    return true;
  }

  int read_dimension(const uvcpp::uvcpp_json& request, const char* key,
                     int fallback) {
    long long value = 0;
    if (!uvcpp::uvcpp_json_get_int(request, key, value)) return fallback;
    if (value <= 0) return fallback;
    if (value > 1000) return 1000;
    return static_cast<int>(value);
  }

  void handle_open(const uvcpp::uvcpp_json& request) {
    const int cols = read_dimension(request, "cols", 80);
    const int rows = read_dimension(request, "rows", 24);
    std::string shell =
        uvcpp::uvcpp_json_get_string(request, "shell", std::string());
    if (shell.size() > 4096) {
      send_error("bad_terminal_request", "shell command is too long");
      return;
    }
    std::string err;
    const uint64_t id = service_->open(viewer_, shell, cols, rows, err);
    if (id == 0) {
      // A Windows core that could not reach a session helper reports the
      // failure with this prefix: the page must say "terminal unavailable"
      // rather than "could not open", because retrying will not help until
      // the service/helper is installed or running.
      const bool unavailable =
          err.rfind("terminal_unavailable", 0) == 0 || !pty_supported();
      send_error(unavailable ? "terminal_unavailable"
                             : "terminal_open_failed",
                 err);
      return;
    }
    attached_.insert(id);
    uvcpp::uvcpp_json reply;
    reply["type"] = "term-opened";
    reply["id"] = id;
    send_json(reply);
  }

  void handle_attach(const uvcpp::uvcpp_json& request) {
    uint64_t id = 0;
    if (!read_id(request, id)) return;
    long long raw_seq = 0;
    uint64_t from_seq = 0;
    if (uvcpp::uvcpp_json_get_int(request, "seq", raw_seq) && raw_seq > 0) {
      from_seq = static_cast<uint64_t>(raw_seq);
    }
    std::vector<uint8_t> replay;
    uint64_t replay_seq = 0;
    bool truncated = false;
    std::string err;
    if (!service_->attach(viewer_, id, from_seq, replay, replay_seq, truncated,
                          err)) {
      send_error(err == "terminal not found" ? "term_not_found" : "term_limit",
                 err, id);
      return;
    }
    attached_.insert(id);
    // Receipt for the attach: `seq` is the first replayed byte and
    // `truncated` says the requested offset was already evicted from the
    // 1 MiB ring, so the replay starts at the oldest retained byte. It goes
    // before the replay frames so the page can show the gap notice first.
    uvcpp::uvcpp_json receipt;
    receipt["type"] = "term-attached";
    receipt["id"] = id;
    receipt["seq"] = replay_seq;
    receipt["truncated"] = truncated;
    send_json(receipt);
    send_replay(id, replay_seq, replay);
    int code = 0;
    if (service_->exit_code(id, code)) {
      attached_.erase(id);
      uvcpp::uvcpp_json exit;
      exit["type"] = "term-exit";
      exit["id"] = id;
      exit["code"] = code;
      send_json(exit);
    }
  }

  void handle_resize(const uvcpp::uvcpp_json& request) {
    uint64_t id = 0;
    if (!read_id(request, id)) return;
    const int cols = read_dimension(request, "cols", 80);
    const int rows = read_dimension(request, "rows", 24);
    if (attached_.count(id) == 0 || !service_->resize(id, cols, rows)) {
      send_error("term_not_found", "terminal is not open", id);
    }
  }

  void handle_close(const uvcpp::uvcpp_json& request) {
    uint64_t id = 0;
    if (!read_id(request, id)) return;
    if (attached_.erase(id) == 0) {
      send_error("term_not_found", "terminal is not open", id);
      return;
    }
    service_->close(id, "viewer");
  }

  uvcpp::uvcpp_ws_connection* conn_ = nullptr;
  std::shared_ptr<terminal_service> service_;
  uint64_t viewer_ = 0;
  uint64_t listener_ = 0;
  std::set<uint64_t> attached_;
  bool closed_ = false;
};

}  // namespace

uvcpp::uvcpp_web_ws_handler make_terminal_handler(
    std::shared_ptr<terminal_service> service,
    std::shared_ptr<auth_state> auth) {
  return [service = std::move(service),
          auth = std::move(auth)](uvcpp::uvcpp_web_ws_request& request) {
    uvcpp::uvcpp_ws_connection* conn = request.connection();
    if (conn == nullptr) return;

    auto reject = [conn, &request](const char* code, const char* reason,
                                   const std::string& message) {
      std::vector<diag_field> fields;
      fields.push_back({"reason", reason});
      fields.push_back({"peer", request.request().peer_ip()});
      fields.push_back({"err_code", code});
      diag().log(diag_level::warn, "term.rejected", message, fields);
      uvcpp::uvcpp_json json;
      json["type"] = "error";
      json["code"] = code;
      if (!message.empty()) json["message"] = message;
      const std::string body = uvcpp::uvcpp_json_dump(json);
      conn->send_text(body.data(), body.size());
      conn->close(uvcpp::ws_close_code::POLICY_VIOLATION, code);
    };

    if (!auth) {
      reject("unauthorized", "auth", "terminal requires authentication");
      return;
    }
    const std::string* token = request.query("token");
    if (token == nullptr || token->empty()) {
      reject("unauthorized", "auth", "a token is required");
      return;
    }
    const login_result result =
        auth->login(*token, std::string(), /*consume=*/true);
    if (!result.ok) {
      reject("unauthorized", "auth", "invalid token");
      return;
    }
    if (!auth->settings().terminal_enabled) {
      reject("terminal_disabled", "disabled",
             "the host has not enabled the remote terminal");
      return;
    }

    auto connection = std::make_shared<terminal_connection>(
        conn, service, service->register_viewer());
    connection->start();
  };
}

}  // namespace molink
