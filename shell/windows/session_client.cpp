#include "session_client.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

#include "error_policy.h"
#include "file_panel_logic.h"
#include "native_diag.h"
#include "puff.h"

namespace molink::net {
namespace {

/// Microsecond clock (QPC): GetTickCount64's ~15.6 ms tick is coarser than a
/// loopback round trip.
/// The host's tile clock grid (protocol: u16 cell, always 128 today).
constexpr int kTileClockCell = 128;

uint64_t now_us() {
  static LARGE_INTEGER frequency{};
  if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  return static_cast<uint64_t>(counter.QuadPart * 1000000 /
                               (frequency.QuadPart > 0 ? frequency.QuadPart : 1));
}

std::wstring wide_of(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                       static_cast<int>(text.size()), nullptr,
                                       0);
  if (size <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(),
                      static_cast<int>(text.size()), out.data(), size);
  return out;
}

uint16_t read_u16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t read_u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t read_u64(const uint8_t* p) {
  uint64_t value = 0;
  for (int i = 7; i >= 0; --i) value = (value << 8) | p[i];
  return value;
}

void write_u32(uint8_t* p, uint32_t value) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(value >> (8 * i));
}

void write_u64(uint8_t* p, uint64_t value) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(value >> (8 * i));
}

std::string utf8_of(const std::wstring& text) {
  if (text.empty()) return std::string();
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                       static_cast<int>(text.size()), nullptr,
                                       0, nullptr, nullptr);
  if (size <= 0) return std::string();
  std::string out(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), size, nullptr, nullptr);
  return out;
}

std::string json_escape(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (unsigned char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
          out += buffer;
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  return out;
}

/// Parses one JSON string starting at *pos (the opening quote), handling
/// escapes; advances *pos past the closing quote.
bool parse_json_string(const std::string& json, size_t* pos, std::string* out) {
  if (*pos >= json.size() || json[*pos] != '"') return false;
  ++*pos;
  out->clear();
  while (*pos < json.size()) {
    const char c = json[(*pos)++];
    if (c == '"') return true;
    if (c != '\\') {
      out->push_back(c);
      continue;
    }
    if (*pos >= json.size()) break;
    const char e = json[(*pos)++];
    switch (e) {
      case '"': out->push_back('"'); break;
      case '\\': out->push_back('\\'); break;
      case '/': out->push_back('/'); break;
      case 'b': out->push_back('\b'); break;
      case 'f': out->push_back('\f'); break;
      case 'n': out->push_back('\n'); break;
      case 'r': out->push_back('\r'); break;
      case 't': out->push_back('\t'); break;
      case 'u': {
        if (*pos + 4 > json.size()) return false;
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
          const char h = json[(*pos)++];
          value <<= 4;
          if (h >= '0' && h <= '9') {
            value |= static_cast<unsigned>(h - '0');
          } else if (h >= 'a' && h <= 'f') {
            value |= static_cast<unsigned>(h - 'a' + 10);
          } else if (h >= 'A' && h <= 'F') {
            value |= static_cast<unsigned>(h - 'A' + 10);
          } else {
            return false;
          }
        }
        if (value < 0x80) {
          out->push_back(static_cast<char>(value));
        } else if (value < 0x800) {
          out->push_back(static_cast<char>(0xc0 | (value >> 6)));
          out->push_back(static_cast<char>(0x80 | (value & 0x3f)));
        } else {
          out->push_back(static_cast<char>(0xe0 | (value >> 12)));
          out->push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
          out->push_back(static_cast<char>(0x80 | (value & 0x3f)));
        }
        break;
      }
      default: out->push_back(e); break;
    }
  }
  return false;
}

void skip_ws(const std::string& json, size_t* pos) {
  while (*pos < json.size() &&
         (json[*pos] == ' ' || json[*pos] == '\t' || json[*pos] == '\n' ||
          json[*pos] == '\r')) {
    ++*pos;
  }
}

bool json_string_field(const std::string& json, const char* key, size_t start,
                       std::string* out, size_t* after = nullptr) {
  const std::string needle = std::string("\"") + key + "\"";
  // The first textual match can be a value (`"action":"tail"`), so keep
  // looking until a match is followed by ':' and parses as a string.
  size_t at = start;
  while ((at = json.find(needle, at)) != std::string::npos) {
    size_t pos = at + needle.size();
    skip_ws(json, &pos);
    if (pos < json.size() && json[pos] == ':') {
      ++pos;
      skip_ws(json, &pos);
      if (!parse_json_string(json, &pos, out)) return false;
      if (after != nullptr) *after = pos;
      return true;
    }
    at += needle.size();
  }
  return false;
}

/// String field inside a nested object, e.g. `"tiles": {"backend": "dxgi"}`.
/// The search starts at the object's opening brace. Welcome has exactly one
/// `tiles` object, so the first match is the one the Web reads.
bool json_object_string_field(const std::string& json, const char* object,
                              const char* key, std::string* out) {
  const std::string needle = std::string("\"") + object + "\"";
  size_t at = 0;
  while ((at = json.find(needle, at)) != std::string::npos) {
    size_t pos = at + needle.size();
    skip_ws(json, &pos);
    if (pos < json.size() && json[pos] == ':') {
      ++pos;
      skip_ws(json, &pos);
      if (pos < json.size() && json[pos] == '{') {
        return json_string_field(json, key, pos + 1, out);
      }
    }
    at += needle.size();
  }
  return false;
}

bool json_u64_field(const std::string& json, const char* key, size_t start,
                    uint64_t* out) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t at = start;
  while ((at = json.find(needle, at)) != std::string::npos) {
    size_t pos = at + needle.size();
    skip_ws(json, &pos);
    if (pos >= json.size() || json[pos] != ':') {
      at += needle.size();
      continue;
    }
    ++pos;
    skip_ws(json, &pos);
    uint64_t value = 0;
    bool any = false;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
      value = value * 10 + static_cast<uint64_t>(json[pos] - '0');
      ++pos;
      any = true;
    }
    if (!any) return false;
    *out = value;
    return true;
  }
  return false;
}

std::wstring wbase_name(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

/// Host error codes name their feature. Logging under that component keeps a
/// "why did this fail" searchable by channel; unknown codes stay `session`.
molink::native_diag::component diag_component_for(const std::string& code) {
  using molink::native_diag::component;
  if (code == "unauthorized" || code == "invalid_token" ||
      code == "invalid_code" || code == "too_many_attempts" ||
      code == "ip_banned" || code == "locked" || code == "access_denied") {
    return component::auth;
  }
  if (code == "bad_input" || code.rfind("input_", 0) == 0) {
    return component::input;
  }
  if (code.rfind("clipboard_", 0) == 0 ||
      code.rfind("remote_clipboard_", 0) == 0) {
    return component::clipboard;
  }
  if (code == "bad_file_action" || code.rfind("file_", 0) == 0) {
    return component::files;
  }
  if (code == "bad_preset" || code == "bad_quality" || code == "bad_fps" ||
      code == "bad_qp" || code == "bad_bitrate" ||
      code == "bad_rate_mode" || code == "bad_format" ||
      code == "bad_display" || code == "bad_display_mode" ||
      code.rfind("display_mode_", 0) == 0) {
    return component::video;
  }
  return component::session;
}

/// Splits `"key": [ {...}, {...} ]` into one substring per object. Names may
/// contain quotes and braces, so the scanner walks strings instead of using
/// raw find(). Nested arrays are not expected in the file protocol.
std::vector<std::string> json_object_array(const std::string& text,
                                           const char* key) {
  std::vector<std::string> out;
  const size_t at = text.find(std::string("\"") + key + "\"");
  const size_t array = at == std::string::npos ? std::string::npos
                                               : text.find('[', at);
  if (array == std::string::npos) return out;
  size_t pos = array + 1;
  while (pos < text.size()) {
    skip_ws(text, &pos);
    if (pos < text.size() && text[pos] == ',') {
      ++pos;
      continue;
    }
    if (pos >= text.size() || text[pos] != '{') break;
    const size_t begin = pos;
    bool in_string = false;
    bool escaped = false;
    while (pos < text.size()) {
      const char c = text[pos++];
      if (in_string) {
        if (escaped) {
          escaped = false;
        } else if (c == '\\') {
          escaped = true;
        } else if (c == '"') {
          in_string = false;
        }
      } else if (c == '"') {
        in_string = true;
      } else if (c == '}') {
        break;
      }
    }
    out.push_back(text.substr(begin, pos - begin));
  }
  return out;
}

bool json_bool_field(const std::string& json, const char* key, size_t start,
                     bool* out) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t at = start;
  while ((at = json.find(needle, at)) != std::string::npos) {
    size_t pos = at + needle.size();
    skip_ws(json, &pos);
    if (pos >= json.size() || json[pos] != ':') {
      at += needle.size();
      continue;
    }
    ++pos;
    skip_ws(json, &pos);
    if (json.compare(pos, 4, "true") == 0) {
      *out = true;
      return true;
    }
    if (json.compare(pos, 5, "false") == 0) {
      *out = false;
      return true;
    }
    return false;
  }
  return false;
}

/// Decodes one tile into BGRA. Handles the pixel codecs the host uses for
/// tiles; stream codecs (H.264/VP9/AV1) need a real video decoder and are
/// ignored by this prototype.
bool decode_tile(uint8_t codec, const uint8_t* data, size_t size, int width,
                 int height, std::vector<uint8_t>& bgra) {
  if (width <= 0 || height <= 0) return false;
  const size_t pixels = static_cast<size_t>(width) * height;
  size_t need = 0;
  bool deflated = false;
  switch (codec) {
    case 1:  // bgra
    case 5:  // deflate-bgra
      need = pixels * 4;
      deflated = codec == 5;
      break;
    case 6:  // rgb24
    case 8:  // deflate-rgb24
      need = pixels * 3;
      deflated = codec == 8;
      break;
    case 7:  // rgb565
    case 9:  // deflate-rgb565
      need = pixels * 2;
      deflated = codec == 9;
      break;
    case 10:  // gray8
    case 11:  // deflate-gray8
      need = pixels;
      deflated = codec == 11;
      break;
    default:
      return false;
  }
  std::vector<uint8_t> inflated;
  const uint8_t* src = data;
  if (deflated) {
    inflated.resize(need);
    unsigned long dest_len = static_cast<unsigned long>(need);
    // The host's deflated tiles are zlib streams (RFC 1950); puff() decodes
    // raw DEFLATE only, so skip the 2-byte header (the Adler-32 trailer is
    // simply left unconsumed).
    size_t skip = 0;
    if (size >= 2 && (data[0] & 0x0f) == 8 &&
        ((static_cast<unsigned>(data[0]) << 8) | data[1]) % 31 == 0) {
      skip = 2;
    }
    unsigned long src_len = static_cast<unsigned long>(size - skip);
    if (puff(inflated.data(), &dest_len, data + skip, &src_len) != 0 ||
        dest_len != need) {
      return false;
    }
    src = inflated.data();
  } else if (size < need) {
    return false;
  }

  bgra.resize(pixels * 4);
  for (size_t i = 0; i < pixels; ++i) {
    uint8_t b = 0, g = 0, r = 0;
    switch (codec) {
      case 1:
      case 5:
        bgra[i * 4 + 0] = src[i * 4 + 0];
        bgra[i * 4 + 1] = src[i * 4 + 1];
        bgra[i * 4 + 2] = src[i * 4 + 2];
        bgra[i * 4 + 3] = src[i * 4 + 3];
        continue;
      case 6:
      case 8:
        r = src[i * 3 + 0];
        g = src[i * 3 + 1];
        b = src[i * 3 + 2];
        break;
      case 7:
      case 9: {
        const uint16_t value = read_u16(src + i * 2);
        b = static_cast<uint8_t>((value & 0x1f) << 3);
        g = static_cast<uint8_t>(((value >> 5) & 0x3f) << 2);
        r = static_cast<uint8_t>(((value >> 11) & 0x1f) << 3);
        break;
      }
      default:
        r = g = b = src[i];
        break;
    }
    bgra[i * 4 + 0] = b;
    bgra[i * 4 + 1] = g;
    bgra[i * 4 + 2] = r;
    bgra[i * 4 + 3] = 255;
  }
  return true;
}

std::string quality_path(const session_endpoint& endpoint) {
  if (endpoint.direct_session) return "/ws/session";
  std::string path;
  if (!endpoint.target.empty()) {
    path = "/ws/connect?target=" + endpoint.target;
  } else {
    path = "/ws/direct?host=" + endpoint.host +
           "&port=" + std::to_string(endpoint.port);
  }
  if (!endpoint.client_token.empty()) {
    path += "&client=" + endpoint.client_token;
  }
  return path;
}

}  // namespace

bool session_client::find_number(const std::string& json, const char* key,
                                 int* out) {
  const std::string needle = std::string("\"") + key + "\":";
  const size_t at = json.find(needle);
  if (at == std::string::npos) return false;
  size_t i = at + needle.size();
  while (i < json.size() && (json[i] == ' ' || json[i] == '"')) ++i;
  int value = 0;
  bool any = false;
  while (i < json.size() && json[i] >= '0' && json[i] <= '9') {
    value = value * 10 + (json[i] - '0');
    ++i;
    any = true;
  }
  if (!any) return false;
  *out = value;
  return true;
}

bool session_client::find_u64(const std::string& json, const char* key,
                              uint64_t* out) {
  const std::string needle = std::string("\"") + key + "\":";
  const size_t at = json.find(needle);
  if (at == std::string::npos) return false;
  size_t i = at + needle.size();
  while (i < json.size() && (json[i] == ' ' || json[i] == '"')) ++i;
  uint64_t value = 0;
  bool any = false;
  while (i < json.size() && json[i] >= '0' && json[i] <= '9') {
    value = value * 10 + static_cast<uint64_t>(json[i] - '0');
    ++i;
    any = true;
  }
  if (!any) return false;
  *out = value;
  return true;
}

bool session_client::find_string(const std::string& json, const char* key,
                                 std::string* out) {
  const std::string needle = std::string("\"") + key + "\":\"";
  const size_t at = json.find(needle);
  if (at == std::string::npos) return false;
  const size_t start = at + needle.size();
  const size_t end = json.find('"', start);
  if (end == std::string::npos) return false;
  *out = json.substr(start, end - start);
  return true;
}

bool session_client::start(const std::string& core_host, int core_port,
                           const session_endpoint& endpoint,
                           ui::session_stats* stats, std::string* error) {
  stats_ = stats;
  authenticated_ = false;
  access_pending_ = false;
  tiles_requested_ = false;
  has_image_.store(false);
  bootstrap_requests_ = 0;
  last_bootstrap_at_ = 0;
  stat_bytes_.store(0);
  stat_frames_ = 0;
  stat_window_start_ = GetTickCount64();
  last_rtt_ms_ = 0;
  cursor_visible_.store(false);
  cursor_x_.store(0);
  cursor_y_.store(0);
  applied_cells_.clear();
  last_clock_request_ = 0;
  last_clock_seen_.store(0);
  last_refresh_at_ = 0;
  if (stats_ != nullptr) {
    stats_->fps = 0.0;
    stats_->last_frame_ms = 0;
  }
  last_error_.clear();
  last_error_code_.clear();
  last_feature_error_.clear();
  last_feature_error_code_.clear();
  {
    std::lock_guard<std::mutex> lock(log_mutex_);
    host_logs_ = host_log_state{};
    host_logs_.port = endpoint.port;
    tiles_backend_.clear();
    host_version_.clear();
  }
  video_mode_.clear();
  avatar_.clear();
  close_detail_.clear();
  device_name_.clear();
  security_.clear();
  input_available_.store(false);
  path_label_.clear();
  protocol_label_.clear();
  // A new socket starts with a clean file-transfer state; partial downloads
  // from the previous session are discarded.
  for (auto& [download_id, item] : downloads_) {
    item.out.close();
    if (!item.path.empty()) DeleteFileW(item.path.c_str());
  }
  downloads_.clear();
  uploads_.clear();
  upload_slots_.clear();
  // A new socket owns a fresh clipboard-file state. Partial pulls from the
  // previous connection are deleted, and echoes from it must not suppress a
  // genuine copy in this one.
  discard_clipboard_pull();
  clipboard_offered_.clear();
  clipboard_offer_key_.clear();
  clipboard_echo_.clear();
  clipboard_session_ = "s" + std::to_string(GetCurrentProcessId()) + "-" +
                       std::to_string(GetTickCount64());
  clipboard_batch_seq_ = 0;
  next_clipboard_task_ = 1;
  host_clipboard_stream_ = false;
  host_clipboard_pull_ = false;
  host_nested_rdp_ = false;
  next_offer_key_ = next_upload_key_;
  pending_downloads_.clear();
  request_queue_.clear();
  active_requests_ = 0;
  pending_browses_.clear();
  download_walks_.clear();
  next_walk_id_ = 1;
  active_offer_key_ = 0;
  shell_file_events_.clear();
  {
    std::lock_guard<std::mutex> lock(file_mutex_);
    file_events_.clear();
  }
  ws_.on_text = [this](const std::string& text) { handle_text(text); };
  ws_.on_binary = [this](const uint8_t* data, size_t size) {
    handle_binary(data, size);
  };
  ws_.on_close = [this] {
    // Keep what the peer/network said about the drop: reconnect diagnostics
    // ("going_away: idle timeout", 1006, ...) used to be thrown away.
    {
      const int code = ws_.close_code();
      const std::string& reason = ws_.close_reason();
      if (code != 0 || !reason.empty()) {
        close_detail_ = std::to_wstring(code);
        if (!reason.empty()) close_detail_ += L": " + wide_of(reason);
      } else {
        close_detail_ = L"connection lost (no close frame)";
      }
    }
    molink::native_diag::diag().log(
        molink::native_diag::level::warn,
        molink::native_diag::component::session, "close", utf8_of(close_detail_),
        {{"authenticated", authenticated_ ? "1" : "0"},
         {"ws_code", std::to_string(ws_.close_code())}});
    if (stats_ != nullptr) stats_->codec = L"-";
    // Some transports deliver only the close frame (the web reads the same
    // reasons): surface a policy answer before the session is accepted.
    if (authenticated_ || !last_error_.empty()) return;
    const std::string reason = ws_.close_reason();
    if (reason.empty()) return;
    std::string lower;
    lower.reserve(reason.size());
    for (char c : reason) {
      lower.push_back(
          static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    auto has = [&lower](const char* needle) {
      return lower.find(needle) != std::string::npos;
    };
    std::string code;
    if (has("invalid token") || has("unauthorized") || has("invalid code")) {
      code = "invalid_code";
    } else if (has("locked")) {
      code = "locked";
    } else if (has("denied")) {
      code = "access_denied";
    } else if (has("too many")) {
      code = "too_many_attempts";
    } else if (has("banned")) {
      code = "ip_banned";
    } else if (has("hello") || has("handshake") || has("timed out") ||
               has("timeout")) {
      // The host's 1008 handshake answers ("hello timeout", "invalid hello")
      // belong in the session banner, not in a feature toast.
      code = "handshake_failed";
    } else {
      return;
    }
    last_error_code_ = std::wstring(code.begin(), code.end());
    last_error_ = wide_of(code + ": " + reason);
    error_seq_.fetch_add(1);
    access_pending_ = false;
  };
  if (!ws_.connect(core_host, core_port, quality_path(endpoint), error)) {
    return false;
  }
  hello_resends_ = 0;
  access_request_ = endpoint.token.empty();
  // A resume token is a stronger credential than the code: present it even
  // when the user left the code empty, so an approved (or code-authenticated)
  // session reconnects without another approval or code use.
  if (access_request_ && resume_token_.empty()) {
    // No code and no resume token: the host shows an approval dialog to the
    // person at the machine. The session stays unauthenticated until they
    // accept.
    access_pending_ = true;
    hello_text_ =
        "{\"type\":\"request-access\",\"name\":\"moLink native\","
        "\"client\":\"molink-native\",\"clipboard_stream\":true,"
        "\"clipboard_pull\":true}";
  } else {
    // In surface mode the local core decodes AV1/VP9/VP8 and publishes the
    // native surface, so the stream capabilities are real. A direct client
    // decodes tiles only: claiming streams there would make the host stop
    // sending tile keyframes and leave a blank picture. The `client` field
    // lets the local core clamp these flags to the decoders it actually
    // carries (a viewer build without libvpx must never ask for VP9).
    const char* stream_cap = stream_decoding_ ? "true" : "false";
    hello_text_ =
        "{\"type\":\"hello\",\"client\":\"molink-native\",\"token\":\"" +
        endpoint.token + "\"";
    if (!resume_token_.empty()) {
      // Host session tokens are generated hex; still escape defensively so a
      // malformed token cannot break the hello frame.
      hello_text_ += ",\"session\":\"";
      for (const char c : resume_token_) {
        if (c == '"' || c == '\\') hello_text_.push_back('\\');
        hello_text_.push_back(c);
      }
      hello_text_ += "\"";
    }
    hello_text_ +=
        ",\"seq_clock\":true,\"video_push\":true,\"h264_stream\":false,"
        "\"vp9_stream\":" + std::string(stream_cap) + ",\"vp8_stream\":" +
        stream_cap + ",\"av1_native\":" + stream_cap +
        // This shell can answer a range fetch for a file on the local
        // clipboard (`clipboard-stream`: the host's CF_HDROP/FileContents
        // path) and understands `clipboard-sync.files` + `clipboard-pull`.
        ",\"clipboard_stream\":true,\"clipboard_pull\":true,"
        "\"protocol\":1}";
  }
  hello_sent_at_ = GetTickCount64();
  return ws_.send_text(hello_text_);
}

void session_client::handle_binary(const uint8_t* data, size_t size) {
  if (size < 32 || data[0] != 'M' || data[1] != 'O' || data[2] != 'L' ||
      data[3] != 'K') {
    return;
  }
  if (data[4] != 1) return;
  stat_bytes_.fetch_add(size);
  const uint8_t type = data[5];
  const uint32_t payload_len = read_u32(data + 24);
  if (payload_len > size - 32) return;
  if (type == 0x12) {
    // file_data: u32 id, u32 reserved, u64 offset, then the raw bytes. It
    // enters the same queue as the JSON actions: two queues would let a
    // following `eof` be processed before the final chunk.
    if (payload_len < 16) return;
    const uint8_t* payload = data + 32;
    file_event event;
    event.action = L"chunk";
    event.id = read_u32(payload);
    event.offset = read_u64(payload + 8);
    event.chunk_data.assign(payload + 16, payload + payload_len);
    std::lock_guard<std::mutex> lock(file_mutex_);
    file_events_.push_back(std::move(event));
    return;
  }
  if (type == 0x18) {
    // clipboard_file range reply: u32 id, u32 status, u64 offset, then the
    // bytes (little-endian; host `send_clipboard_file_reply`). It joins the
    // same ordered queue as `clipboard-pull-offer` and file_data, so ranges
    // are written in the order they arrived.
    if (payload_len < 16) return;
    const uint8_t* payload = data + 32;
    file_event event;
    event.action = L"clipboard-pull-chunk";
    event.id = read_u32(payload);
    event.status = read_u32(payload + 4);
    event.offset = read_u64(payload + 8);
    event.chunk_data.assign(payload + 16, payload + payload_len);
    std::lock_guard<std::mutex> lock(file_mutex_);
    file_events_.push_back(std::move(event));
    return;
  }
  if (type == 0x15) {
    // Per-cell freshness map, once a second. It replaces the old
    // "no frame for 700 ms -> re-read the whole screen" poll, which on a
    // static desktop re-sent the entire BGRA picture (megabytes) forever.
    if (!stream_decoding_ && request_tiles_) {
      handle_tile_clock(data + 32, payload_len);
    }
    return;
  }
  if (type != 0x02) return;  // frame packets only beyond file data
  const uint8_t* payload = data + 32;
  if (payload_len < 16) return;
  const uint32_t frame_seq = read_u32(payload);
  const uint64_t frame_ts_us = read_u64(data + 16);
  const uint16_t tile_count = read_u16(payload + 4);
  const uint8_t codec = payload[6];
  const uint8_t frame_flags = payload[7];
  live_codec_.store(codec);
  const int screen_w = read_u16(payload + 8);
  const int screen_h = read_u16(payload + 10);
  const size_t table_end = 16 + static_cast<size_t>(tile_count) * 16;
  if (screen_w <= 0 || screen_h <= 0 || table_end > payload_len) return;

  // Low-delay streams (H.264/VP8/VP9/AV1) are not tiles: this client either
  // gets them decoded by the local core through the surface, or (direct mode)
  // cannot paint them at all. Do not run them through the tile decoder.
  if (codec != 1 && codec != 5 && codec != 6 && codec != 7 && codec != 8 &&
      codec != 9 && codec != 10 && codec != 11) {
    return;
  }
  if (std::getenv("MOLINK_WS_LOG") != nullptr) {
    std::fprintf(stderr,
                 "frame seq=%u tiles=%u codec=%u flags=%u %dx%d payload=%u\n",
                 frame_seq, tile_count, codec, frame_flags, screen_w, screen_h,
                 payload_len);
  }
  std::lock_guard<std::mutex> lock(frame_mutex_);
  if (frame_width_ != screen_w || frame_height_ != screen_h) {
    frame_pixels_.assign(static_cast<size_t>(screen_w) * screen_h * 4, 0);
    frame_width_ = screen_w;
    frame_height_ = screen_h;
    // Cell keys are screen-relative: a resolution change invalidates them.
    applied_cells_.clear();
  } else if ((frame_flags & 0x01) != 0) {
    std::fill(frame_pixels_.begin(), frame_pixels_.end(), 0);
  }

  size_t offset = table_end;
  bool painted = false;
  for (uint16_t i = 0; i < tile_count; ++i) {
    const uint8_t* entry = payload + 16 + static_cast<size_t>(i) * 16;
    const int x = read_u16(entry + 0);
    const int y = read_u16(entry + 2);
    const int w = read_u16(entry + 4);
    const int h = read_u16(entry + 6);
    const uint32_t tile_size = read_u32(entry + 12);
    if (tile_size > payload_len - offset) break;
    std::vector<uint8_t> tile;
    const bool decoded =
        w > 0 && h > 0 &&
        decode_tile(codec, payload + offset, tile_size, w, h, tile);
    if (!decoded && std::getenv("MOLINK_WS_LOG") != nullptr) {
      std::fprintf(stderr, "tile decode failed codec=%u %dx%d bytes=%u\n",
                   codec, w, h, tile_size);
    }
    if (decoded) {
      const int copy_w = (std::min)(w, screen_w - x);
      const int copy_h = (std::min)(h, screen_h - y);
      for (int row = 0; row < copy_h; ++row) {
        std::memcpy(frame_pixels_.data() +
                        (static_cast<size_t>(y + row) * screen_w + x) * 4,
                    tile.data() + static_cast<size_t>(row) * w * 4,
                    static_cast<size_t>(copy_w) * 4);
      }
      note_applied_cells(x, y, copy_w, copy_h, frame_ts_us);
      painted = true;
    }
    offset += tile_size;
  }
  if (painted) {
    frame_sequence_ = frame_seq != 0 ? frame_seq : frame_sequence_ + 1;
    has_image_.store(true);
  }
}

// ---------------------------------------------------------------------------
// Tile clock (0x15): incremental repair, matching the web client.
// ---------------------------------------------------------------------------

void session_client::note_applied_cells(int x, int y, int w, int h,
                                        uint64_t ts_us) {
  if (ts_us == 0 || w <= 0 || h <= 0) return;
  const int col0 = x / kTileClockCell;
  const int row0 = y / kTileClockCell;
  const int col1 = (x + w - 1) / kTileClockCell;
  const int row1 = (y + h - 1) / kTileClockCell;
  for (int row = row0; row <= row1; ++row) {
    for (int col = col0; col <= col1; ++col) {
      uint64_t& slot = applied_cells_[(static_cast<uint64_t>(row) << 32) |
                                      static_cast<uint32_t>(col)];
      if (ts_us > slot) slot = ts_us;
    }
  }
}

void session_client::handle_tile_clock(const uint8_t* payload, uint32_t len) {
  if (len < 8) return;
  const uint16_t cols = read_u16(payload);
  const uint16_t rows = read_u16(payload + 2);
  const uint16_t cell = read_u16(payload + 4);
  if (cols == 0 || rows == 0 || cell != kTileClockCell) return;
  const uint64_t need =
      8 + static_cast<uint64_t>(cols) * static_cast<uint64_t>(rows) * 8;
  if (len < need) return;
  const uint64_t now = GetTickCount64();
  last_clock_seen_.store(now);
  if (!has_image_.load()) {
    // Clocks only flow to a session the host believes has a picture, yet this
    // client has none: the first refresh was lost, or the host started on a
    // static desktop and had no pixels to hand out. The bootstrap loop gives
    // up after 40 tries, so keep asking for a full-screen refresh (which
    // reads the live desktop) instead of sitting on a blank stage forever.
    if (request_tiles_ && screen_w_ > 0 && screen_h_ > 0 &&
        now - last_refresh_at_ >= 2000) {
      last_refresh_at_ = now;
      ws_.send_text(
          "{\"type\":\"request-tiles\",\"tiles\":[{\"x\":0,\"y\":0,"
          "\"w\":" + std::to_string(screen_w_) + ",\"h\":" +
          std::to_string(screen_h_) + "}]}");
    }
    return;
  }
  if (now - last_clock_request_ < 1000) return;

  std::string tiles;
  size_t count = 0;
  size_t stale_cells = 0;
  for (int row = 0; row < rows && count < 64; ++row) {
    // One rectangle per horizontal run of stale cells, like the page: a
    // full-width row costs one rect instead of sixteen.
    int run_start = -1;
    for (int col = 0; col <= cols && count < 64; ++col) {
      bool stale = false;
      if (col < cols) {
        const uint64_t host_ts = read_u64(
            payload + 8 +
            (static_cast<size_t>(row) * cols + static_cast<size_t>(col)) * 8);
        const uint64_t key = (static_cast<uint64_t>(row) << 32) |
                             static_cast<uint32_t>(col);
        const auto it = applied_cells_.find(key);
        const uint64_t applied = it == applied_cells_.end() ? 0 : it->second;
        // The clock and the frame stamps share the capture clock; one second
        // of slack covers a frame that is still in flight.
        stale = host_ts > applied + 1000000ULL;
        if (stale) ++stale_cells;
      }
      if (stale && run_start < 0) run_start = col;
      if (!stale && run_start >= 0) {
        if (!tiles.empty()) tiles += ',';
        tiles += "{\"x\":" + std::to_string(run_start * cell) +
                 ",\"y\":" + std::to_string(row * cell) +
                 ",\"w\":" + std::to_string((col - run_start) * cell) +
                 ",\"h\":" + std::to_string(cell) + "}";
        ++count;
        run_start = -1;
      }
    }
  }
  // A static desktop has no stale cell: send nothing at all. This is what
  // makes the idle watermark drop to RustDesk levels.
  if (tiles.empty()) return;
  if (std::getenv("MOLINK_WS_LOG") != nullptr) {
    const uint64_t host0 = read_u64(payload + 8);
    const auto it0 = applied_cells_.find(0);
    const uint64_t applied0 = it0 == applied_cells_.end() ? 0 : it0->second;
    std::fprintf(stderr,
                 "clock: cell0 host=%llu applied=%llu stale_cells=%zu/%u "
                 "rects=%zu\n",
                 static_cast<unsigned long long>(host0),
                 static_cast<unsigned long long>(applied0), stale_cells,
                 static_cast<unsigned>(cols) * rows, count);
  }
  last_clock_request_ = now;
  ws_.send_text("{\"type\":\"request-tiles\",\"tiles\":[" + tiles + "]}");
}

// ---------------------------------------------------------------------------
// File transfer: JSON `file` actions + file_data (0x12) packets.
// ---------------------------------------------------------------------------

void session_client::handle_file_json(const std::string& text) {
  file_event event;
  std::string action;
  if (!json_string_field(text, "action", 0, &action)) return;
  event.action = wide_of(action);
  uint64_t number = 0;
  if (json_u64_field(text, "id", 0, &number)) {
    event.id = static_cast<uint32_t>(number);
  }
  if (json_u64_field(text, "chunk", 0, &number)) {
    event.chunk = static_cast<uint32_t>(number);
  }
  if (json_u64_field(text, "size", 0, &number)) event.size = number;
  if (json_u64_field(text, "index", 0, &number)) {
    event.index = static_cast<uint32_t>(number);
  }
  if (json_u64_field(text, "length", 0, &number)) event.length = number;
  if (json_u64_field(text, "offset", 0, &number)) event.offset = number;
  bool flag = true;
  if (json_bool_field(text, "ok", 0, &flag)) event.ok = flag;
  flag = false;
  if (json_bool_field(text, "stream", 0, &flag)) event.stream = flag;
  if (json_bool_field(text, "nested_rdp", 0, &flag)) event.clip_nested_rdp = flag;
  std::string value;
  if (json_string_field(text, "name", 0, &value)) event.name = wide_of(value);
  if (json_string_field(text, "path", 0, &value)) event.path = wide_of(value);
  if (json_string_field(text, "message", 0, &value)) {
    event.message = wide_of(value);
  }
  if (action == "listing") {
    if (json_string_field(text, "path", 0, &value)) {
      event.listing_path = wide_of(value);
    }
    if (json_string_field(text, "root", 0, &value)) {
      event.listing_root = wide_of(value);
    }
    if (json_string_field(text, "parent", 0, &value)) {
      event.listing_parent = wide_of(value);
    }
    bool truncated = false;
    if (json_bool_field(text, "truncated", 0, &truncated)) {
      event.truncated = truncated;
    }
    for (const std::string& item : json_object_array(text, "roots")) {
      file_listing_root root;
      if (json_string_field(item, "name", 0, &value)) {
        root.name = wide_of(value);
      }
      if (json_string_field(item, "path", 0, &value)) {
        root.path = wide_of(value);
      }
      if (json_string_field(item, "kind", 0, &value)) {
        root.kind = wide_of(value);
      }
      if (!root.name.empty() || !root.path.empty()) {
        event.roots.push_back(std::move(root));
      }
    }
    // entries: [{name, dir, size, mtime, path, kind}, ...]
    for (const std::string& item : json_object_array(text, "entries")) {
      file_listing_entry entry;
      std::string name;
      if (json_string_field(item, "name", 0, &name)) {
        entry.name = wide_of(name);
      }
      bool dir = false;
      json_bool_field(item, "dir", 0, &dir);
      entry.dir = dir;
      uint64_t size = 0;
      if (json_u64_field(item, "size", 0, &size)) entry.size = size;
      if (json_u64_field(item, "mtime", 0, &size)) entry.mtime = size;
      if (json_string_field(item, "path", 0, &value)) {
        entry.path = wide_of(value);
      }
      if (json_string_field(item, "kind", 0, &value)) {
        entry.kind = wide_of(value);
      }
      bool link = false;
      if (json_bool_field(item, "link", 0, &link)) entry.link = link;
      if (!entry.name.empty()) event.entries.push_back(std::move(entry));
    }
  }
  std::lock_guard<std::mutex> lock(file_mutex_);
  file_events_.push_back(std::move(event));
}

bool session_client::send_file_action(const std::string& action, uint32_t id,
                                      const std::string& extra) {
  std::string text = "{\"type\":\"file\",\"action\":\"" + action + "\"";
  if (id != 0) text += ",\"id\":" + std::to_string(id);
  text += extra;
  text += "}";
  return ws_.send_text(text);
}

bool session_client::send_file_chunk(uint32_t id, uint64_t offset,
                                     const uint8_t* data, size_t len) {
  std::vector<uint8_t> packet(32 + 16 + len, 0);
  packet[0] = 'M';
  packet[1] = 'O';
  packet[2] = 'L';
  packet[3] = 'K';
  packet[4] = 1;      // protocol version
  packet[5] = 0x12;   // packet_type::file_data
  write_u32(packet.data() + 24, static_cast<uint32_t>(16 + len));
  write_u32(packet.data() + 32, id);
  write_u32(packet.data() + 36, 0);
  write_u64(packet.data() + 40, offset);
  if (len > 0) std::memcpy(packet.data() + 48, data, len);
  return ws_.send_binary(packet.data(), packet.size());
}

void session_client::set_download_dir(const std::wstring& dir) {
  download_dir_ = dir;
}

bool session_client::file_offer(const std::wstring& name,
                                const std::wstring& local_path,
                                const std::wstring& destination,
                                uint32_t task) {
  if (!ws_.is_open()) return false;
  WIN32_FILE_ATTRIBUTE_DATA info{};
  if (!GetFileAttributesExW(local_path.c_str(), GetFileExInfoStandard, &info)) {
    return false;
  }
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return false;
  const uint64_t size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) |
                        static_cast<uint64_t>(info.nFileSizeLow);
  if (size > molink::files::kMaxFileBytes) return false;
  const std::wstring offer_name = name.empty() ? local_path : name;
  // A folder walk can overlap a separately selected child, and two clicks can
  // race the same row. Keep one queued/active row per destination name.
  if (!upload_slots_.insert(destination + L'\x1f' + offer_name).second) {
    return false;
  }
  upload_state upload;
  upload.key = next_upload_key_++;
  upload.task = task;
  upload.name = offer_name;
  upload.path = local_path;
  upload.dest = destination;
  upload.size = size;
  upload.state = 0;
  uploads_.emplace(upload.key, std::move(upload));
  molink::native_diag::diag().log(
      molink::native_diag::level::info,
      molink::native_diag::component::files, "offer",
      std::string(), {{"name", utf8_of(offer_name)},
                      {"size", std::to_string(size)},
                      {"dest", utf8_of(destination)}});
  return true;
}

bool session_client::file_mkdir(const std::wstring& relative,
                                const std::wstring& destination,
                                uint32_t task) {
  if (!ws_.is_open() || relative.empty()) return false;
  if (!upload_slots_.insert(destination + L'\x1f' + relative).second) {
    return false;
  }
  upload_state upload;
  upload.key = next_upload_key_++;
  upload.task = task;
  upload.name = relative;
  upload.dest = destination;
  upload.mkdir = true;
  upload.state = 0;
  uploads_.emplace(upload.key, std::move(upload));
  return true;
}

void session_client::file_browse(const std::wstring& path) {
  pending_browses_.push_back(pending_browse{path, false, 0});
  send_file_action("browse", 0,
                   ",\"path\":\"" + json_escape(utf8_of(path)) + "\"");
}

void session_client::file_request(const std::wstring& path, uint32_t task) {
  queue_download_request(path, std::wstring(), task);
}

bool session_client::file_request_relative(const std::wstring& path,
                                           const std::wstring& relative,
                                           uint32_t task) {
  return queue_download_request(path, relative, task);
}

bool session_client::queue_download_request(const std::wstring& path,
                                            const std::wstring& relative,
                                            uint32_t task) {
  if (!ws_.is_open() || path.empty()) return false;
  // One row per remote file: an overlapping selection (a folder plus a file
  // inside it) must not request the same bytes twice.
  for (const pending_download& pending : pending_downloads_) {
    if (pending.path == path) return false;
  }
  for (const pending_download& pending : request_queue_) {
    if (pending.path == path) return false;
  }
  for (const auto& [id, download] : downloads_) {
    if (download.state == 1 && download.remote_path == path) return false;
  }
  request_queue_.push_back({path, relative, task});
  pump_download_requests();
  molink::native_diag::diag().log(
      molink::native_diag::level::info,
      molink::native_diag::component::files, "request", std::string(),
      {{"name", utf8_of(path)}, {"relative", utf8_of(relative)}});
  return true;
}

void session_client::pump_download_requests() {
  // The host keeps eight downloads open; asking for a ninth answers with
  // `too many active downloads`, so the queue is what makes a folder with
  // more than eight files work. Web's download queue does the same.
  while (active_requests_ < static_cast<int>(
                                 molink::files::kMaxActiveTransfers) &&
         !request_queue_.empty()) {
    pending_download request = std::move(request_queue_.front());
    request_queue_.pop_front();
    ++active_requests_;
    pending_downloads_.push_back(std::move(request));
    const pending_download& sent = pending_downloads_.back();
    send_file_action("request", 0,
                     ",\"path\":\"" + json_escape(utf8_of(sent.path)) +
                         "\"");
  }
}

bool session_client::file_download_tree(const std::wstring& path,
                                        const std::wstring& relative,
                                        uint32_t task) {
  if (!ws_.is_open() || path.empty() || !ensure_download_root()) return false;
  const std::string rel_utf8 = molink::files::normalize_relative_path(
      utf8_of(relative));
  if (!molink::files::is_safe_relative_path(rel_utf8)) return false;
  const uint32_t id = next_walk_id_++;
  download_walk walk;
  walk.task = task;
  walk.pending.push_back({path, wide_of(rel_utf8)});
  download_walks_.emplace(id, std::move(walk));
  advance_download_walk(id);
  return true;
}

session_client::pending_browse session_client::take_pending_browse() {
  pending_browse result;
  if (pending_browses_.empty()) return result;
  result = pending_browses_.front();
  pending_browses_.pop_front();
  return result;
}

void session_client::advance_download_walk(uint32_t walk_id) {
  auto it = download_walks_.find(walk_id);
  if (it == download_walks_.end()) return;
  download_walk& walk = it->second;
  if (walk.next >= walk.pending.size()) {
    file_event done;
    done.action = L"walk-done";
    done.task = walk.task;
    done.walk_files = walk.files;
    done.walk_dirs = walk.dirs;
    done.walk_skipped = walk.skipped;
    done.truncated = walk.truncated;
    shell_file_events_.push_back(std::move(done));
    download_walks_.erase(it);
    return;
  }
  const download_walk::folder& folder = walk.pending[walk.next];
  ensure_local_directory(folder.rel);
  pending_browses_.push_back(pending_browse{folder.path, true, walk_id});
  send_file_action("browse", 0,
                   ",\"path\":\"" + json_escape(utf8_of(folder.path)) + "\"");
}

void session_client::handle_walk_listing(const file_event& event,
                                         uint32_t walk_id) {
  auto it = download_walks_.find(walk_id);
  if (it == download_walks_.end()) return;
  download_walk& walk = it->second;
  if (walk.next < walk.pending.size()) {
    const std::wstring rel = walk.pending[walk.next].rel;
    ensure_local_directory(rel);
  }
  if (!event.ok) {
    ++walk.skipped;
  } else {
    if (event.truncated) walk.truncated = true;
    const std::wstring listed_path =
        event.listing_path.empty() && walk.next < walk.pending.size()
            ? walk.pending[walk.next].path
            : event.listing_path;
    const std::wstring base_rel =
        walk.next < walk.pending.size() ? walk.pending[walk.next].rel
                                        : std::wstring();
    for (const file_listing_entry& entry : event.entries) {
      if (walk.entries >= molink::files::kMaxWalkEntries) {
        walk.truncated = true;
        break;
      }
      ++walk.entries;
      const std::string name_utf8 = utf8_of(entry.name);
      // The host's answer is untrusted: one ordinary segment or nothing.
      if (!molink::files::is_safe_relative_path(name_utf8) ||
          name_utf8.find('/') != std::string::npos ||
          name_utf8.find('\\') != std::string::npos) {
        ++walk.skipped;
        continue;
      }
      const std::wstring child_rel =
          base_rel.empty() ? entry.name : base_rel + L"/" + entry.name;
      const std::wstring child_path = wide_of(molink::files::join_path(
          utf8_of(listed_path), name_utf8));
      if (entry.dir) {
        ++walk.dirs;
        ensure_local_directory(child_rel);
        if (entry.link) continue;
        const int depth = static_cast<int>(
            std::count(child_rel.begin(), child_rel.end(), L'/')) + 1;
        if (depth > molink::files::kMaxWalkDepth) {
          walk.truncated = true;
          continue;
        }
        walk.pending.push_back({child_path, child_rel});
      } else {
        ++walk.files;
        queue_download_request(child_path, child_rel, walk.task);
      }
    }
  }
  ++walk.next;
  advance_download_walk(walk_id);
}

void session_client::file_cancel(uint32_t id) {
  if ((id & kClipboardRowFlag) != 0) {
    // Synthetic row id from `file_transfers()` for an inbound clipboard pull.
    cancel_clipboard_pull();
    return;
  }
  if (id == 0) {
    for (auto& [key, upload] : uploads_) {
      if (upload.state == 0 || upload.state == 1) {
        if (upload.host_id != 0) {
          send_file_action("cancel", upload.host_id, "");
        }
        upload.in.close();
        release_upload_slot(upload);
        upload.state = 3;
        upload.cancelled = true;
        upload.message = L"已取消";
        if (active_offer_key_ == key) active_offer_key_ = 0;
        return;
      }
    }
    return;
  }
  for (auto& [key, upload] : uploads_) {
    if (upload.host_id == id && upload.state != 2) {
      send_file_action("cancel", id, "");
      upload.in.close();
      release_upload_slot(upload);
      upload.state = 3;
      upload.cancelled = true;
      upload.message = L"已取消";
      if (active_offer_key_ == key) active_offer_key_ = 0;
      return;
    }
  }
  auto it = downloads_.find(id);
  if (it != downloads_.end()) {
    send_file_action("cancel", id, "");
    it->second.out.close();
    if (!it->second.path.empty()) DeleteFileW(it->second.path.c_str());
    if (it->second.state == 1 && active_requests_ > 0) --active_requests_;
    it->second.state = 3;
    it->second.cancelled = true;
    it->second.message = L"已取消";
  }
}

namespace {

/// Unique local path for a download under `dir`.
std::wstring unique_download_path(const std::wstring& dir,
                                  const std::wstring& name) {
  std::wstring clean;
  for (wchar_t c : name) {
    if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' ||
        c == L'"' || c == L'<' || c == L'>' || c == L'|' || c < 32) {
      continue;
    }
    clean.push_back(c);
  }
  if (clean.empty()) clean = L"file";
  std::wstring candidate = dir + L"\\" + clean;
  if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return candidate;
  }
  const size_t dot = clean.find_last_of(L'.');
  const std::wstring stem = dot == std::wstring::npos ? clean
                                                      : clean.substr(0, dot);
  const std::wstring ext =
      dot == std::wstring::npos ? L"" : clean.substr(dot);
  for (int i = 1; i < 10000; ++i) {
    candidate = dir + L"\\" + stem + L" (" + std::to_wstring(i) + L")" + ext;
    if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) {
      return candidate;
    }
  }
  return dir + L"\\" + std::to_wstring(GetTickCount64()) + L"-" + clean;
}

}  // namespace

void session_client::release_upload_slot(const upload_state& upload) {
  upload_slots_.erase(upload.dest + L'\x1f' + upload.name);
}

bool session_client::ensure_download_root() {
  if (download_dir_.empty()) {
    wchar_t profile[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) > 0) {
      download_dir_ = std::wstring(profile) + L"\\Downloads\\moLink";
    }
  }
  if (download_dir_.empty()) return false;
  CreateDirectoryW(download_dir_.c_str(), nullptr);
  return true;
}

bool session_client::ensure_local_directory(const std::wstring& relative) {
  if (!ensure_download_root()) return false;
  const std::string rel = molink::files::normalize_relative_path(
      utf8_of(relative));
  if (rel.empty()) return true;
  // `safe_target_segments` drops `..`: a hostile peer name can never escape
  // the download root even when it is used as a folder.
  const std::vector<std::string> segments =
      molink::files::safe_target_segments(rel, "folder");
  std::wstring path = download_dir_;
  for (const std::string& segment : segments) {
    path += L"\\";
    path += wide_of(segment);
    CreateDirectoryW(path.c_str(), nullptr);
  }
  return true;
}

std::wstring session_client::local_download_path(const std::wstring& relative,
                                                 std::wstring* display_name) {
  const std::string rel = molink::files::normalize_relative_path(
      utf8_of(relative));
  if (rel.empty()) return std::wstring();
  if (!ensure_download_root()) return std::wstring();
  const std::vector<std::string> segments =
      molink::files::safe_target_segments(rel, "file");
  if (segments.empty()) return std::wstring();
  std::wstring path = download_dir_;
  std::wstring shown;
  for (std::size_t i = 0; i < segments.size(); ++i) {
    path += L"\\";
    path += wide_of(segments[i]);
    if (!shown.empty()) shown.push_back(L'/');
    shown += wide_of(segments[i]);
    if (i + 1 < segments.size()) CreateDirectoryW(path.c_str(), nullptr);
  }
  if (display_name != nullptr) *display_name = shown;
  return path;
}

void session_client::finish_download(uint32_t id) {
  auto it = downloads_.find(id);
  if (it == downloads_.end()) return;
  download_state& download = it->second;
  if (download.state != 1 || !download.eof_seen ||
      download.received != download.size) {
    return;
  }
  download.out.close();
  download.state = 2;
  if (active_requests_ > 0) --active_requests_;
  file_event done;
  done.action = L"downloaded";
  done.id = download.id;
  done.size = download.size;
  done.name = download.name;
  done.path = download.path;
  shell_file_events_.push_back(std::move(done));
}

void session_client::pump_files() {
  // 0. Keep the host's eight download slots full (a recursive download can
  // queue thousands of files long before their answers arrive).
  pump_download_requests();
  // 1. Offer the next queued file/folder when nothing is in flight. Keys are
  // allocated in order and complete in order, so a cursor walks the queue in
  // O(1) per item instead of rescanning a 20000-entry map.
  if (active_offer_key_ == 0) {
    while (next_offer_key_ < next_upload_key_) {
      const auto it = uploads_.find(next_offer_key_);
      if (it == uploads_.end() ||
          it->second.state == 2 || it->second.state == 3) {
        ++next_offer_key_;
        continue;
      }
      break;
    }
    auto next = uploads_.find(next_offer_key_);
    if (next != uploads_.end() && next->second.state == 0 &&
        !next->second.offer_sent) {
      upload_state& upload = next->second;
      upload.offer_sent = true;
      upload.sent_at = GetTickCount64();
      active_offer_key_ = next_offer_key_;
      next_offer_key_ = next->first + 1;
      if (upload.mkdir) {
        upload.state = 1;
        send_file_action("mkdir", 0,
                         ",\"path\":\"" + json_escape(utf8_of(upload.name)) +
                             "\",\"dest\":\"" +
                             json_escape(utf8_of(upload.dest)) + "\"");
      } else {
        send_file_action(
            "offer", 0,
            ",\"name\":\"" + json_escape(utf8_of(upload.name)) +
                "\",\"size\":" + std::to_string(upload.size) +
                ",\"dest\":\"" + json_escape(utf8_of(upload.dest)) + "\"");
      }
    }
  }

  // 2. Drain host messages.
  std::vector<file_event> incoming;
  {
    std::lock_guard<std::mutex> lock(file_mutex_);
    incoming.swap(file_events_);
  }
  // Each `sending` answer belongs to the oldest request still waiting for one;
  // a name match picks the right entry if a rejected request (host id 0) made
  // the queue skip. The host answers requests in order.
  auto take_pending_download = [&](const std::wstring& name) {
    pending_download result;
    if (pending_downloads_.empty()) return result;
    size_t index = 0;
    if (!name.empty()) {
      for (size_t i = 0; i < pending_downloads_.size(); ++i) {
        if (wbase_name(pending_downloads_[i].path) == name) {
          index = i;
          break;
        }
      }
    }
    result = pending_downloads_[index];
    pending_downloads_.erase(
        pending_downloads_.begin() + static_cast<std::ptrdiff_t>(index));
    return result;
  };
  for (file_event& event : incoming) {
    const std::wstring& action = event.action;
    if (action == L"clipboard-pull-offer") {
      start_clipboard_pull(event);
      continue;
    }
    if (action == L"clipboard-pull-chunk") {
      handle_clipboard_chunk(event);
      continue;
    }
    if (action == L"fetch") {
      handle_clipboard_fetch(event);
      continue;
    }
    if (action == L"clipboard-offer" && (!event.ok || !event.stream)) {
      // The host rejected the delayed offer (or would need the eager path).
      // Forget the dedup key so copying the same files again retries.
      clipboard_files_cleared();
    }
    if (action == L"listing") {
      // `browse` answers arrive in request order, so the FIFO tells a
      // recursive download's listing apart from a panel navigation.
      const pending_browse pending = take_pending_browse();
      if (pending.walk) {
        handle_walk_listing(event, pending.walk_id);
        continue;
      }
    }
    if (action == L"mkdir") {
      auto it = uploads_.find(active_offer_key_);
      if (it != uploads_.end() && it->second.mkdir) {
        upload_state& upload = it->second;
        upload.state = event.ok ? 2 : 3;
        if (!event.ok) {
          upload.message = event.message.empty() ? L"无法创建远程文件夹"
                                                 : event.message;
        }
        event.task = upload.task;
        event.name = upload.name;
        if (event.ok) {
          if (event.path.empty()) event.path = upload.name;
        } else {
          event.message = upload.message;
        }
        active_offer_key_ = 0;
        shell_file_events_.push_back(std::move(event));
        continue;
      }
    }
    if (action == L"chunk") {
      auto it = downloads_.find(event.id);
      if (std::getenv("MOLINK_WS_LOG") != nullptr) {
        std::fprintf(stderr,
                     "file rx chunk id=%u off=%llu len=%zu state=%d "
                     "received=%llu known=%d\n",
                     event.id,
                     static_cast<unsigned long long>(event.offset),
                     event.chunk_data.size(),
                     it == downloads_.end() ? -1 : it->second.state,
                     it == downloads_.end()
                         ? 0ull
                         : static_cast<unsigned long long>(
                               it->second.received),
                     it == downloads_.end() ? 0 : 1);
      }
      if (it == downloads_.end()) continue;
      download_state& download = it->second;
      if (download.state != 1) continue;
      if (event.offset != download.received || download.received > download.size ||
          event.chunk_data.size() > download.size - download.received) {
        download.out.close();
        if (!download.path.empty()) DeleteFileW(download.path.c_str());
        download.state = 3;
        download.message = L"文件接收不完整";
        if (active_requests_ > 0) --active_requests_;
        continue;
      }
      download.out.write(
          reinterpret_cast<const char*>(event.chunk_data.data()),
          static_cast<std::streamsize>(event.chunk_data.size()));
      download.received += event.chunk_data.size();
      if (!event.chunk_data.empty()) download.last_progress_at = GetTickCount64();
      if (!download.out) {
        download.out.close();
        DeleteFileW(download.path.c_str());
        download.state = 3;
        download.message = L"无法写入本地文件";
        if (active_requests_ > 0) --active_requests_;
        continue;
      }
      if (download.received < download.size && !download.eof_seen) {
        send_file_action("pull", download.id, "");
      }
      finish_download(event.id);
      continue;
    }
    if (action == L"accept") {
      auto it = uploads_.find(active_offer_key_);
      if (it != uploads_.end() && it->second.host_id == 0) {
        upload_state& upload = it->second;
        upload.host_id = event.id;
        if (event.chunk > 0) upload.chunk = event.chunk;
        upload.state = 1;
        upload.sent = 0;
        upload.in.open(upload.path.c_str(), std::ios::binary);
        if (!upload.in) {
          send_file_action("cancel", upload.host_id, "");
          release_upload_slot(upload);
          upload.state = 3;
          upload.message = L"无法读取本地文件";
          event.task = upload.task;
          event.message = upload.message;
          shell_file_events_.push_back(event);
          continue;
        }
      }
      continue;
    }
    if (action == L"done" || action == L"reject" || action == L"cancel" ||
        action == L"cancelled" || action == L"error") {
      // Match by host id; `reject` may arrive before an id is assigned.
      upload_state* target = nullptr;
      for (auto& [key, upload] : uploads_) {
        if (upload.host_id != 0 && upload.host_id == event.id &&
            upload.state != 2 && upload.state != 3) {
          target = &upload;
          break;
        }
      }
      if (target == nullptr && event.id == 0) {
        auto it = uploads_.find(active_offer_key_);
        if (it != uploads_.end() && it->second.state != 2 &&
            it->second.state != 3) {
          target = &it->second;
        }
      }
      if (target != nullptr) {
        target->in.close();
        release_upload_slot(*target);
        if (event.name.empty()) event.name = target->name;
        event.task = target->task;
        if (action == L"done" && event.ok) {
          target->state = 2;
          target->saved_path = event.path;
        } else {
          target->state = 3;
          target->cancelled =
              action == L"cancel" || action == L"cancelled";
          target->message = event.message;
          if (target->message.empty()) {
            target->message =
                target->cancelled ? L"已取消" : L"传输失败";
          }
          event.message = target->message;
        }
        if (uploads_.find(active_offer_key_) != uploads_.end()) {
          active_offer_key_ = 0;
        }
      }
      shell_file_events_.push_back(std::move(event));
      continue;
    }
    if (action == L"sending") {
      const pending_download pending = take_pending_download(event.name);
      if (std::getenv("MOLINK_WS_LOG") != nullptr) {
        std::fprintf(stderr, "file rx sending id=%u size=%llu ok=%d\n",
                     event.id, static_cast<unsigned long long>(event.size),
                     event.ok ? 1 : 0);
      }
      if (!event.ok) {
        // The host refused the request (`not a file`, too large, 8 slots
        // busy...). Keep a failed row so the task summary and the reason are
        // visible instead of only a transient toast.
        download_state failed_state;
        failed_state.id = event.id;
        failed_state.task = pending.task;
        failed_state.rel = pending.rel;
        failed_state.remote_path = pending.path;
        failed_state.name =
            !pending.rel.empty()
                ? pending.rel
                : (event.name.empty() ? wbase_name(pending.path) : event.name);
        if (failed_state.name.empty()) failed_state.name = L"file";
        failed_state.size = event.size;
        failed_state.state = 3;
        failed_state.message =
            event.message.empty() ? L"文件接收失败" : event.message;
        // Host ids start at 1; keep synthetic failed rows in a separate range
        // so they can never overwrite a real download with the same number.
        const uint32_t key =
            event.id != 0 ? event.id
                          : (0x80000000u | next_download_key_++);
        downloads_[key] = std::move(failed_state);
        if (active_requests_ > 0) --active_requests_;
        event.task = pending.task;
        shell_file_events_.push_back(std::move(event));
        continue;
      }
      download_state download;
      download.id = event.id;
      download.task = pending.task;
      download.rel = pending.rel;
      download.remote_path = pending.path;
      download.name = event.name.empty() ? L"file" : event.name;
      download.size = event.size;
      download.state = 1;
      download.last_progress_at = GetTickCount64();
      const bool rooted = ensure_download_root();
      if (!pending.rel.empty()) {
        // Recursive download: recreate the remote tree under the download
        // directory. `local_download_path` sanitises peer-supplied names.
        std::wstring shown;
        const std::wstring target =
            rooted ? local_download_path(pending.rel, &shown) : std::wstring();
        if (target.empty()) {
          download.state = 3;
          download.message = L"无法创建本地文件";
        } else {
          download.path = target;
          download.name = shown;
        }
      } else if (rooted) {
        download.path = unique_download_path(download_dir_, download.name);
      } else {
        download.state = 3;
        download.message = L"无法创建本地文件";
      }
      if (download.state != 1) {
        if (active_requests_ > 0) --active_requests_;
        file_event failed;
        failed.action = L"error";
        failed.id = download.id;
        failed.task = download.task;
        failed.name = download.name;
        failed.message = download.message;
        downloads_[download.id] = std::move(download);
        shell_file_events_.push_back(std::move(failed));
        continue;
      }
      download.out.open(download.path.c_str(),
                        std::ios::binary | std::ios::trunc);
      if (!download.out) {
        download.state = 3;
        download.message = L"无法创建本地文件";
        if (active_requests_ > 0) --active_requests_;
        file_event failed;
        failed.action = L"error";
        failed.id = download.id;
        failed.task = download.task;
        failed.name = download.name;
        failed.message = download.message;
        downloads_[download.id] = std::move(download);
        shell_file_events_.push_back(std::move(failed));
        continue;
      }
      downloads_[download.id] = std::move(download);
      shell_file_events_.push_back(event);
      send_file_action("pull", event.id, "");
      continue;
    }
    if (action == L"eof") {
      auto it = downloads_.find(event.id);
      if (std::getenv("MOLINK_WS_LOG") != nullptr) {
        std::fprintf(stderr, "file rx eof id=%u received=%llu size=%llu\n",
                     event.id,
                     it == downloads_.end()
                         ? 0ull
                         : static_cast<unsigned long long>(
                               it->second.received),
                     it == downloads_.end()
                         ? 0ull
                         : static_cast<unsigned long long>(it->second.size));
      }
      if (it != downloads_.end() && it->second.state == 1) {
        download_state& download = it->second;
        if (!event.ok) {
          download.out.close();
          DeleteFileW(download.path.c_str());
          download.state = 3;
          download.message =
              event.message.empty() ? L"文件接收失败" : event.message;
          if (active_requests_ > 0) --active_requests_;
          event.action = L"error";
          event.name = download.name;
          event.task = download.task;
          event.message = download.message;
          shell_file_events_.push_back(std::move(event));
          continue;
        }
        // The host may send `eof` on the control path before the last binary
        // frame is delivered. Remember it and finish once the bytes catch up.
        download.eof_seen = true;
        download.eof_at = GetTickCount64();
        finish_download(event.id);
      }
      continue;
    }
    shell_file_events_.push_back(std::move(event));
  }

  // A finished download frees a host slot: start the next queued request
  // before the timeout pass so a large folder keeps moving.
  pump_download_requests();
  // Clipboard-file ranges are independent of the host's eight file-transfer
  // slots; drive them from the UI timer as well so a lost 0x18 is retried.
  pump_clipboard_pull(GetTickCount64());

  // A missing chunk or end marker must not leave the row spinning forever.
  const uint64_t now_ms = GetTickCount64();
  for (auto& [download_id, download] : downloads_) {
    if (download.state != 1) continue;
    const bool timed_out = download.eof_seen
        ? now_ms - download.eof_at > 3000
        : now_ms - download.last_progress_at > 30000;
    if (!timed_out) continue;
    send_file_action("cancel", download_id, "");
    download.out.close();
    DeleteFileW(download.path.c_str());
    download.state = 3;
    download.message = L"文件接收不完整";
    if (active_requests_ > 0) --active_requests_;
    file_event failed;
    failed.action = L"error";
    failed.id = download.id;
    failed.task = download.task;
    failed.name = download.name;
    failed.message = download.message;
    shell_file_events_.push_back(std::move(failed));
  }

  // A lost `mkdir` answer must not hold the single active-upload slot.
  for (auto& [key, upload] : uploads_) {
    if (!upload.mkdir || upload.state != 1) continue;
    if (now_ms - upload.sent_at <= 30000) continue;
    upload.state = 3;
    upload.message = L"创建远程文件夹超时";
    if (active_offer_key_ == key) active_offer_key_ = 0;
    file_event failed;
    failed.action = L"error";
    failed.task = upload.task;
    failed.name = upload.name;
    failed.message = upload.message;
    shell_file_events_.push_back(std::move(failed));
  }

  // 3. Send the active upload in bounded slices (UI thread, no worker).
  auto active = uploads_.find(active_offer_key_);
  if (active == uploads_.end() || active->second.state != 1 ||
      active->second.mkdir) {
    return;
  }
  upload_state& upload = active->second;
  // The local core proxy can deliver a small JSON `complete` ahead of the
  // last large binary frame. Send at most two chunks per tick and hold
  // `complete` until the following tick, like the web's setTimeout(0) yield.
  if (upload.complete_pending && !upload.complete_sent) {
    upload.complete_pending = false;
    upload.complete_sent = true;
    if (std::getenv("MOLINK_WS_LOG") != nullptr) {
      std::fprintf(stderr, "file complete id=%u\n", upload.host_id);
    }
    send_file_action("complete", upload.host_id, "");
    return;
  }
  int slices = 2;
  while (slices-- > 0 && upload.sent < upload.size) {
    const size_t want = static_cast<size_t>(
        (std::min<uint64_t>)(upload.chunk, upload.size - upload.sent));
    const size_t slice = want;
    std::vector<uint8_t> buffer(slice);
    upload.in.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(slice));
    const std::streamsize got = upload.in.gcount();
    const bool sent_ok =
        got > 0 && send_file_chunk(upload.host_id, upload.sent, buffer.data(),
                                   static_cast<size_t>(got));
    if (std::getenv("MOLINK_WS_LOG") != nullptr) {
      std::fprintf(stderr, "file chunk id=%u off=%llu len=%lld ok=%d\n",
                   upload.host_id,
                   static_cast<unsigned long long>(upload.sent),
                   static_cast<long long>(got), sent_ok ? 1 : 0);
    }
    if (!sent_ok) {
      upload.in.close();
      upload.state = 3;
      upload.message = L"发送中断";
      active_offer_key_ = 0;
      return;
    }
    upload.sent += static_cast<uint64_t>(got);
  }
  if (upload.sent >= upload.size && !upload.complete_sent) {
    upload.complete_pending = true;
  }
}

std::vector<file_event> session_client::take_file_events() {
  std::vector<file_event> out;
  out.swap(shell_file_events_);
  return out;
}

std::vector<file_transfer_state> session_client::file_transfers() {
  std::vector<file_transfer_state> out;
  for (const auto& [key, upload] : uploads_) {
    file_transfer_state row;
    row.id = upload.host_id;
    row.task = upload.task;
    row.name = upload.name;
    row.size = upload.size;
    row.sent = upload.sent;
    row.download = false;
    row.mkdir = upload.mkdir;
    row.state = upload.state;
    row.cancelled = upload.cancelled;
    row.message = upload.message;
    out.push_back(std::move(row));
  }
  for (const auto& [id, download] : downloads_) {
    file_transfer_state row;
    row.id = download.id;
    row.task = download.task;
    row.name = download.name;
    row.size = download.size;
    row.sent = download.received;
    row.download = true;
    row.state = download.state;
    row.cancelled = download.cancelled;
    row.message = download.message;
    out.push_back(std::move(row));
  }
  // Inbound clipboard files share the drawer's progress rows. Their ids are
  // synthetic (the host ids restart at 1 for every selection) and carry
  // `kClipboardRowFlag`, which `file_cancel` routes to the pull batch.
  if (clipboard_pull_ != nullptr) {
    const clipboard_pull_batch& batch = *clipboard_pull_;
    for (std::size_t i = 0; i < batch.files.size(); ++i) {
      const clipboard_pull_file& file = batch.files[i];
      file_transfer_state row;
      row.id = kClipboardRowFlag | static_cast<uint32_t>(i + 1);
      row.task = file.task;
      row.name = L"剪贴板 · " + wide_of(file.name);
      row.size = file.size;
      row.sent = file.received;
      row.download = true;
      row.state = file.state;
      row.cancelled = file.cancelled;
      row.message = file.message;
      out.push_back(std::move(row));
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Clipboard files: inbound pulls (clipboard-sync.files + clipboard-pull/0x18)
// and outbound delayed offers (clipboard-offer + fetch/0x17).
// ---------------------------------------------------------------------------

std::wstring session_client::clipboard_base_directory() const {
  wchar_t local[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) > 0) {
    return std::wstring(local) + L"\\moLink";
  }
  wchar_t profile[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) > 0) {
    return std::wstring(profile) + L"\\AppData\\Local\\moLink";
  }
  return std::wstring();
}

std::wstring session_client::clipboard_batch_directory(uint32_t sequence) const {
  const std::wstring base = clipboard_base_directory();
  if (base.empty()) return std::wstring();
  const std::string relative = molink::clipboard_files::staging_relative_directory(
      clipboard_session_, sequence);
  std::wstring path = base;
  // The module returns UTF-8 segments separated by '/'; create every level so
  // `%LOCALAPPDATA%\moLink\clipboard\<session>\<sequence>` exists before the
  // first `.part` file is opened.
  size_t start = 0;
  while (start <= relative.size()) {
    const size_t slash = relative.find('/', start);
    const size_t end = slash == std::string::npos ? relative.size() : slash;
    const std::string segment = relative.substr(start, end - start);
    if (!segment.empty()) {
      path += L"\\";
      path += wide_of(segment);
      CreateDirectoryW(path.c_str(), nullptr);
    }
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return path;
}

void session_client::queue_clipboard_toast(const wchar_t* action,
                                           const std::wstring& message,
                                           int count) {
  file_event event;
  event.action = action;
  event.message = message;
  event.count = count;
  shell_file_events_.push_back(std::move(event));
}

bool session_client::set_clipboard_files(
    const std::vector<std::wstring>& paths) {
  if (paths.empty()) return false;
  std::size_t characters = 1;  // final list terminator
  for (const std::wstring& path : paths) characters += path.size() + 1;
  const std::size_t bytes =
      sizeof(DROPFILES) + characters * sizeof(wchar_t);
  for (int attempt = 0; attempt < 5; ++attempt) {
    if (!OpenClipboard(nullptr)) {
      Sleep(10);
      continue;
    }
    EmptyClipboard();
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory != nullptr) {
      auto* drop = static_cast<DROPFILES*>(GlobalLock(memory));
      if (drop != nullptr) {
        ZeroMemory(drop, sizeof(DROPFILES));
        drop->pFiles = sizeof(DROPFILES);
        drop->fWide = TRUE;
        auto* cursor = reinterpret_cast<wchar_t*>(
            reinterpret_cast<BYTE*>(drop) + sizeof(DROPFILES));
        for (const std::wstring& path : paths) {
          std::memcpy(cursor, path.c_str(), path.size() * sizeof(wchar_t));
          cursor += path.size();
          *cursor++ = L'\0';
        }
        *cursor = L'\0';
        GlobalUnlock(memory);
        if (SetClipboardData(CF_HDROP, memory) != nullptr) {
          CloseClipboard();
          return true;
        }
      }
      GlobalFree(memory);
    }
    CloseClipboard();
  }
  return false;
}

bool session_client::send_clipboard_range(uint32_t id, uint32_t code,
                                          uint64_t offset,
                                          const uint8_t* data, size_t len) {
  std::vector<uint8_t> packet(32 + 16 + len, 0);
  packet[0] = 'M';
  packet[1] = 'O';
  packet[2] = 'L';
  packet[3] = 'K';
  packet[4] = 1;     // protocol version
  packet[5] = 0x17;  // packet_type::clipboard_range
  write_u32(packet.data() + 24, static_cast<uint32_t>(16 + len));
  write_u32(packet.data() + 32, id);
  write_u32(packet.data() + 36, code);
  write_u64(packet.data() + 40, offset);
  if (len != 0 && data != nullptr) {
    std::memcpy(packet.data() + 48, data, len);
  }
  return ws_.send_binary(packet.data(), packet.size());
}

void session_client::discard_clipboard_pull() {
  if (!clipboard_pull_) return;
  for (clipboard_pull_file& file : clipboard_pull_->files) {
    if (file.out.is_open()) file.out.close();
    if (!file.temp_path.empty()) DeleteFileW(file.temp_path.c_str());
    // A completed batch is referenced by the local clipboard; its final
    // files must outlive the selection so an Explorer paste still works.
    if (file.state != 2 && !file.final_path.empty()) {
      DeleteFileW(file.final_path.c_str());
    }
  }
  clipboard_pull_.reset();
}

bool session_client::clipboard_files_changed(
    const std::vector<std::string>& paths) {
  if (!ws_.is_open() || !authenticated_ || paths.empty()) return false;
  // The Web only uses delayed rendering when the host echoed the capability;
  // an older host would need the eager upload path (not implemented here).
  if (!host_clipboard_stream_) {
    queue_clipboard_toast(L"clipboard-warn",
                          L"远端不支持剪贴板文件，未发送");
    return false;
  }

  const uint64_t now = GetTickCount64();
  std::vector<molink::clipboard_files::entry> raw;
  std::vector<uint64_t> stamps;
  std::size_t skipped = 0;
  for (const std::string& utf8_path : paths) {
    if (utf8_path.empty()) continue;
    if (clipboard_echo_.suppress_path(utf8_path, now)) continue;
    const std::wstring wide_path = wide_of(utf8_path);
    if (wide_path.empty()) {
      ++skipped;
      continue;
    }
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(wide_path.c_str(), GetFileExInfoStandard,
                              &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      // A folder copy is not a file list; the host's lazy path expects files.
      ++skipped;
      continue;
    }
    molink::clipboard_files::entry file;
    file.id = static_cast<std::uint32_t>(raw.size() + 1);
    file.name = utf8_of(wbase_name(wide_path));
    file.size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) |
                static_cast<uint64_t>(info.nFileSizeLow);
    file.path = utf8_path;
    raw.push_back(std::move(file));
    // Re-copying a file whose contents changed but whose name and size did
    // not is still a new selection. The Web keys on the local core's content
    // hash; the mtime is the native equivalent here.
    stamps.push_back((static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime)
                      << 32) |
                     static_cast<uint64_t>(info.ftLastWriteTime.dwLowDateTime));
  }

  const molink::clipboard_files::plan plan =
      molink::clipboard_files::plan_offer_entries(raw);
  const std::size_t omitted = plan.omitted + skipped;
  if (plan.files.empty()) {
    if (raw.empty() && skipped > 0) {
      // Explorer can copy folders too; the host's lazy file list has no
      // directory entry the remote apps could paste.
      queue_clipboard_toast(L"clipboard-warn",
                            L"剪贴板中的文件夹暂不支持发送");
    } else if (omitted > 0) {
      queue_clipboard_toast(
          L"clipboard-warn",
          L"剪贴板文件超出上限，未发送给远端");
    }
    return false;
  }

  std::string key;
  for (std::size_t i = 0; i < plan.files.size(); ++i) {
    const molink::clipboard_files::entry& file = plan.files[i];
    if (!key.empty()) key.push_back('|');
    key += file.name + ':' + std::to_string(file.size) + ':' +
           std::to_string(i < stamps.size() ? stamps[i] : 0);
  }
  if (key == clipboard_offer_key_) return false;

  std::string files_json;
  for (const molink::clipboard_files::entry& file : plan.files) {
    if (!files_json.empty()) files_json.push_back(',');
    files_json += "{\"name\":\"";
    files_json += json_escape(file.name);
    files_json += "\",\"size\":" + std::to_string(file.size) + "}";
  }
  const std::string extra =
      ",\"files\":[" + files_json +
      "],\"dest\":\"~/moLink/clipboard-lazy\"";
  // docs/protocol.md §文件传输: a viewer's own selection is announced with
  // `clipboard-offer`; `clipboard-sync` is the host -> viewer direction.
  if (!send_file_action("clipboard-offer", 0, extra)) return false;

  clipboard_offered_.clear();
  for (const molink::clipboard_files::entry& file : plan.files) {
    clipboard_offered_file offered;
    offered.name = file.name;
    offered.path = wide_of(file.path);
    offered.size = file.size;
    clipboard_offered_.push_back(std::move(offered));
    clipboard_echo_.remember_descriptor(file.name, file.size, now);
  }
  clipboard_offer_key_ = key;
  if (omitted > 0) {
    queue_clipboard_toast(
        L"clipboard-warn",
        L"剪贴板文件超出上限，只发送了 " +
            std::to_wstring(plan.files.size()) + L" 个");
  }
  molink::native_diag::diag().log(
      molink::native_diag::level::info,
      molink::native_diag::component::clipboard, "offer", std::string(),
      {{"files", std::to_string(plan.files.size())},
       {"omitted", std::to_string(omitted)}});
  return true;
}

void session_client::clipboard_files_cleared() {
  // Keep `clipboard_offered_` answerable: the host may already be inside a
  // paste whose fetch is in flight. Only the dedup key is dropped, so the
  // same files may be offered again once the clipboard holds them again.
  clipboard_offer_key_.clear();
}

void session_client::start_clipboard_pull(const file_event& offer) {
  const uint64_t now = GetTickCount64();
  if (offer.clip_files.empty()) {
    // The remote clipboard became text (or was cleared): the announced ids no
    // longer name anything, so abandon the batch and delete its partials.
    discard_clipboard_pull();
    return;
  }

  std::vector<molink::clipboard_files::entry> raw;
  raw.reserve(offer.clip_files.size());
  for (const molink::clipboard_files::entry& file : offer.clip_files) {
    if (clipboard_echo_.suppress_descriptor(file.name, file.size, now)) {
      continue;
    }
    raw.push_back(file);
  }
  if (raw.empty()) return;  // our own offer came back

  const molink::clipboard_files::plan plan =
      molink::clipboard_files::plan_pull_entries(raw);
  const std::size_t omitted = plan.omitted + offer.clip_omitted;
  if (plan.files.empty()) {
    // The whole new selection is over a limit (or malformed). It supersedes
    // whatever was in flight, exactly like a new valid selection does.
    discard_clipboard_pull();
    if (omitted > 0) {
      queue_clipboard_toast(
          L"clipboard-warn",
          L"远端剪贴板有 " + std::to_wstring(omitted) +
              L" 个文件超出上限，未接收");
    }
    return;
  }

  // A new real selection supersedes the previous one. Do this only after the
  // echo filter, so a host echo of our own offer cannot cancel an unrelated
  // download that is already running.
  discard_clipboard_pull();

  const uint32_t sequence = ++clipboard_batch_seq_;
  std::wstring directory = clipboard_batch_directory(sequence);
  auto batch = std::make_unique<clipboard_pull_batch>();
  batch->sequence = sequence;
  batch->total = plan.bytes;
  batch->omitted = omitted;
  batch->nested_rdp = offer.clip_nested_rdp;

  if (directory.empty()) {
    queue_clipboard_toast(L"clipboard-error",
                          L"无法创建剪贴板目录，未接收文件");
    return;
  }
  batch->directory = directory;
  clipboard_pull_ = std::move(batch);
  molink::native_diag::diag().log(
      molink::native_diag::level::info,
      molink::native_diag::component::clipboard, "pull.start", std::string(),
      {{"files", std::to_string(plan.files.size())},
       {"bytes", std::to_string(plan.bytes)},
       {"omitted", std::to_string(omitted)}});

  for (const molink::clipboard_files::entry& source : plan.files) {
    clipboard_pull_file file;
    file.id = source.id;
    file.task = kClipboardTaskFlag | (next_clipboard_task_++);
    file.name = source.name;
    file.size = source.size;
    file.final_path = directory + L"\\" + wide_of(source.name);
    file.temp_path = file.final_path + L".part";
    file.out.open(file.temp_path.c_str(),
                  std::ios::binary | std::ios::trunc);
    if (!file.out) {
      fail_clipboard_pull(L"无法写入剪贴板文件");
      return;
    }
    if (file.size == 0) {
      file.out.close();
      file.state = 2;  // an empty file needs no range request
    }
    clipboard_pull_->files.push_back(std::move(file));
  }

  if (offer.clip_nested_rdp) {
    queue_clipboard_toast(L"clipboard-info",
                          L"远端剪贴板来自内层 RDP 会话");
  }
  if (omitted > 0) {
    queue_clipboard_toast(
        L"clipboard-warn",
        L"远端剪贴板另有 " + std::to_wstring(omitted) +
            L" 个文件超出上限，未接收");
  }
  pump_clipboard_pull(now);
}

void session_client::request_clipboard_range(clipboard_pull_file& file,
                                             uint64_t now_ms) {
  const uint64_t length =
      molink::clipboard_files::chunk_length(file.size, file.received);
  if (length == 0) {
    file.state = 2;
    if (file.out.is_open()) file.out.close();
    return;
  }
  file.state = 1;
  file.requested_at = now_ms;
  if (!send_file_action("clipboard-pull", file.id,
                        ",\"offset\":" + std::to_string(file.received) +
                            ",\"length\":" + std::to_string(length))) {
    // The socket is gone; waiting out the range timeout would only delay the
    // user-visible failure.
    fail_clipboard_pull(L"连接已断开，剪贴板文件接收失败");
  }
}

void session_client::advance_clipboard_pull(uint64_t now_ms) {
  // A finished/invalid active file releases the slot first.
  if (clipboard_pull_->active >= 0) {
    clipboard_pull_file& active =
        clipboard_pull_->files[static_cast<std::size_t>(
            clipboard_pull_->active)];
    if (active.state == 1) return;  // waiting for its range reply
    clipboard_pull_->active = -1;
  }
  for (std::size_t i = 0; i < clipboard_pull_->files.size(); ++i) {
    clipboard_pull_file& file = clipboard_pull_->files[i];
    if (file.state != 0) continue;
    if (file.size == 0 || file.received >= file.size) {
      if (file.out.is_open()) file.out.close();
      file.state = 2;
      continue;
    }
    clipboard_pull_->active = static_cast<int>(i);
    request_clipboard_range(file, now_ms);
    return;
  }
  bool all_done = true;
  for (const clipboard_pull_file& file : clipboard_pull_->files) {
    if (file.state != 2) {
      all_done = false;
      break;
    }
  }
  if (all_done) finish_clipboard_pull();
}

void session_client::pump_clipboard_pull(uint64_t now_ms) {
  if (!clipboard_pull_ || clipboard_pull_->failed ||
      clipboard_pull_->cancelled) {
    return;
  }
  if (clipboard_pull_->active >= 0) {
    clipboard_pull_file& file = clipboard_pull_->files[static_cast<std::size_t>(
        clipboard_pull_->active)];
    if (file.state == 1) {
      if (file.received >= file.size) {
        if (file.out.is_open()) file.out.close();
        file.state = 2;
        clipboard_pull_->active = -1;
      } else if (now_ms - file.requested_at >=
                 molink::clipboard_files::range_timeout_ms) {
        if (file.retry.failed()) {
          request_clipboard_range(file, now_ms);
        } else {
          fail_clipboard_pull(L"剪贴板文件接收超时");
        }
        return;
      } else {
        return;
      }
    } else {
      clipboard_pull_->active = -1;
    }
  }
  advance_clipboard_pull(now_ms);
}

void session_client::handle_clipboard_chunk(const file_event& event) {
  if (!clipboard_pull_ || clipboard_pull_->failed ||
      clipboard_pull_->cancelled || clipboard_pull_->active < 0) {
    return;
  }
  clipboard_pull_file& file = clipboard_pull_->files[static_cast<std::size_t>(
      clipboard_pull_->active)];
  if (file.id != event.id || file.state != 1) return;
  // A timed-out range that was re-requested can still produce an earlier
  // reply. Once a newer reply has advanced the file, a success that ends at
  // or before the current offset is a stale duplicate, not a short read.
  if (event.offset < file.received) {
    if (event.status == 1) return;
    if (event.status == 0 &&
        event.offset + event.chunk_data.size() <= file.received) {
      return;
    }
  }

  const uint64_t expected =
      molink::clipboard_files::chunk_length(file.size, file.received);
  const molink::clipboard_files::chunk_state check =
      molink::clipboard_files::check_chunk(
          event.status, event.offset, file.received, expected,
          event.chunk_data.size());
  switch (check) {
    case molink::clipboard_files::chunk_state::ok:
      break;
    case molink::clipboard_files::chunk_state::gone:
      fail_clipboard_pull(L"远端剪贴板已变化，请重新复制");
      return;
    case molink::clipboard_files::chunk_state::incomplete:
      fail_clipboard_pull(L"剪贴板文件接收不完整");
      return;
    case molink::clipboard_files::chunk_state::failed:
      if (file.retry.failed()) {
        request_clipboard_range(file, GetTickCount64());
      } else {
        fail_clipboard_pull(L"剪贴板文件接收失败");
      }
      return;
  }

  if (!event.chunk_data.empty()) {
    file.out.write(
        reinterpret_cast<const char*>(event.chunk_data.data()),
        static_cast<std::streamsize>(event.chunk_data.size()));
    if (!file.out) {
      fail_clipboard_pull(L"无法写入剪贴板文件");
      return;
    }
  }
  file.received += event.chunk_data.size();
  clipboard_pull_->received += event.chunk_data.size();
  file.retry.succeeded();
  if (file.received >= file.size) {
    file.out.close();
    if (!file.out) {
      fail_clipboard_pull(L"无法写入剪贴板文件");
      return;
    }
    file.state = 2;
    clipboard_pull_->active = -1;
  } else {
    request_clipboard_range(file, GetTickCount64());
    return;
  }
  pump_clipboard_pull(GetTickCount64());
}

void session_client::finish_clipboard_pull() {
  if (!clipboard_pull_ || clipboard_pull_->complete ||
      clipboard_pull_->failed || clipboard_pull_->cancelled) {
    return;
  }
  std::vector<std::wstring> finals;
  finals.reserve(clipboard_pull_->files.size());
  for (clipboard_pull_file& file : clipboard_pull_->files) {
    if (file.state != 2 || file.received != file.size) {
      fail_clipboard_pull(L"剪贴板文件接收不完整");
      return;
    }
    if (file.out.is_open()) file.out.close();
    if (!MoveFileExW(file.temp_path.c_str(), file.final_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING)) {
      fail_clipboard_pull(L"无法保存剪贴板文件");
      return;
    }
    finals.push_back(file.final_path);
  }

  const uint64_t now = GetTickCount64();
  // Record the paths before the clipboard write: the resulting
  // WM_CLIPBOARDUPDATE must be recognised as an echo, not a new local copy.
  for (const std::wstring& path : finals) {
    clipboard_echo_.remember_path(utf8_of(path), now);
  }
  if (!set_clipboard_files(finals)) {
    fail_clipboard_pull(L"无法写入本机剪贴板");
    return;
  }
  clipboard_pull_->complete = true;
  clipboard_pull_->received = clipboard_pull_->total;
  // The local clipboard now describes the host's selection, so a later copy
  // of a previous outbound selection must be treated as a fresh offer.
  clipboard_offer_key_.clear();
  molink::native_diag::diag().log(
      molink::native_diag::level::info,
      molink::native_diag::component::clipboard, "pull.done", std::string(),
      {{"files", std::to_string(finals.size())},
       {"bytes", std::to_string(clipboard_pull_->total)}});
  queue_clipboard_toast(
      L"clipboard-done",
      L"已接收 " + std::to_wstring(finals.size()) + L" 个剪贴板文件",
      static_cast<int>(finals.size()));
}

void session_client::fail_clipboard_pull(const std::wstring& message) {
  if (!clipboard_pull_) return;
  clipboard_pull_->failed = true;
  clipboard_pull_->active = -1;
  for (clipboard_pull_file& file : clipboard_pull_->files) {
    if (file.out.is_open()) file.out.close();
    if (!file.temp_path.empty()) DeleteFileW(file.temp_path.c_str());
    if (!file.final_path.empty()) DeleteFileW(file.final_path.c_str());
    file.state = 3;
    file.message = message;
  }
  molink::native_diag::diag().log(
      molink::native_diag::level::warn,
      molink::native_diag::component::clipboard, "pull.fail",
      utf8_of(message));
  queue_clipboard_toast(L"clipboard-error", message);
}

void session_client::cancel_clipboard_pull() {
  if (!clipboard_pull_ || clipboard_pull_->complete) return;
  clipboard_pull_->cancelled = true;
  clipboard_pull_->active = -1;
  for (clipboard_pull_file& file : clipboard_pull_->files) {
    if (file.out.is_open()) file.out.close();
    if (!file.temp_path.empty()) DeleteFileW(file.temp_path.c_str());
    if (!file.final_path.empty()) DeleteFileW(file.final_path.c_str());
    if (file.state != 2) {
      file.state = 3;
      file.cancelled = true;
      file.message = L"已取消";
    }
  }
  queue_clipboard_toast(L"clipboard-warn", L"已取消接收剪贴板文件");
}

void session_client::handle_clipboard_fetch(const file_event& event) {
  if (!authenticated_) return;
  // Prefer the name over the index: a newer local copy may have replaced the
  // offered list between the host's paste and this fetch, and serving the
  // file now at that index would paste the wrong bytes under a still-valid
  // request id. The planner makes names unique within one offer.
  std::size_t index = clipboard_offered_.size();
  const std::string name = utf8_of(event.name);
  if (!name.empty()) {
    for (std::size_t i = 0; i < clipboard_offered_.size(); ++i) {
      if (clipboard_offered_[i].name == name) {
        index = i;
        break;
      }
    }
    if (index >= clipboard_offered_.size()) {
      // The host still has a request for an offer this machine replaced. Do
      // not serve whatever now sits at that index.
      send_clipboard_range(event.id, 1, event.offset, nullptr, 0);
      return;
    }
  } else if (event.index < clipboard_offered_.size()) {
    index = event.index;
  }
  if (index >= clipboard_offered_.size()) {
    send_clipboard_range(event.id, 1, event.offset, nullptr, 0);
    return;
  }
  const clipboard_offered_file& file = clipboard_offered_[index];
  uint64_t offset = event.offset;
  uint64_t length = event.length;
  if (length == 0) length = molink::clipboard_files::chunk_bytes;
  if (offset > file.size) {
    send_clipboard_range(event.id, 1, offset, nullptr, 0);
    return;
  }
  uint64_t take = file.size - offset;
  take = (std::min)(take, length);
  take = (std::min)(take, molink::clipboard_files::chunk_bytes);
  if (take == 0) {
    send_clipboard_range(event.id, 0, offset, nullptr, 0);
    return;
  }

  std::ifstream in(file.path.c_str(), std::ios::binary);
  if (!in) {
    send_clipboard_range(event.id, 1, offset, nullptr, 0);
    return;
  }
  in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  std::vector<uint8_t> data(static_cast<std::size_t>(take));
  if (!in) {
    send_clipboard_range(event.id, 1, offset, nullptr, 0);
    return;
  }
  in.read(reinterpret_cast<char*>(data.data()),
          static_cast<std::streamsize>(data.size()));
  const std::streamsize got = in.gcount();
  if (got != static_cast<std::streamsize>(data.size())) {
    send_clipboard_range(event.id, 1, offset, nullptr, 0);
    return;
  }
  send_clipboard_range(event.id, 0, offset, data.data(), data.size());
}

std::vector<std::string> session_client::take_chat() {
  std::lock_guard<std::mutex> lock(inbox_mutex_);
  std::vector<std::string> out;
  out.swap(chat_inbox_);
  return out;
}

std::vector<std::string> session_client::take_clipboard() {
  std::lock_guard<std::mutex> lock(inbox_mutex_);
  std::vector<std::string> out;
  out.swap(clipboard_inbox_);
  return out;
}

bool session_client::copy_frame(std::vector<uint8_t>& out, int* width,
                                int* height, uint64_t* sequence,
                                uint64_t since) {
  std::lock_guard<std::mutex> lock(frame_mutex_);
  if (frame_sequence_ == 0 || frame_sequence_ == since ||
      frame_width_ <= 0 || frame_height_ <= 0) {
    return false;
  }
  out = frame_pixels_;
  if (width != nullptr) *width = frame_width_;
  if (height != nullptr) *height = frame_height_;
  if (sequence != nullptr) *sequence = frame_sequence_;
  note_displayed_frame();
  if (stats_ != nullptr) stats_->note_frame(GetTickCount64());
  return true;
}

void session_client::note_displayed_frame() { ++stat_frames_; }

void session_client::note_surface_frame() { has_image_.store(true); }

void session_client::handle_text(const std::string& text) {
  if (stats_ == nullptr) return;
  std::string type;
  if (!find_string(text, "type", &type)) return;
  if (type == "cursor-position") {
    int x = 0;
    int y = 0;
    if (find_number(text, "x", &x) && find_number(text, "y", &y)) {
      cursor_x_.store(x);
      cursor_y_.store(y);
    }
    cursor_visible_.store(text.find("\"visible\":true") != std::string::npos);
    return;
  }
  if (type == "ping") {
    // The host probes liveness with its own JSON ping; the page answers it.
    uint64_t stamp = 0;
    if (find_u64(text, "t", &stamp)) {
      ws_.send_text("{\"type\":\"pong\",\"t\":" + std::to_string(stamp) + "}");
    }
    return;
  }
  if (type == "welcome") {
    authenticated_ = true;
    access_pending_ = false;
    authenticated_at_ = GetTickCount64();
    extra_frame_requested_ = false;
    std::string preference;
    const bool has_preference =
        find_string(text, "codec_preference", &preference) &&
        !preference.empty();
    if (has_preference) {
      stats_->codec = std::wstring(preference.begin(), preference.end());
    }
    std::string video_mode;
    if (find_string(text, "video_mode", &video_mode) && !video_mode.empty()) {
      video_mode_ = wide_of(video_mode);
    }
    std::string session_token;
    if (find_string(text, "session_token", &session_token) &&
        !session_token.empty()) {
      resume_token_ = session_token;
    }
    std::string avatar;
    if (find_string(text, "avatar", &avatar) && !avatar.empty()) {
      avatar_ = wide_of(avatar);
    }
    // The viewer-owned preset goes out before the first frame, like the Web
    // (SessionView sends it as soon as the session is live). Sending it once
    // per connection also covers the reconnect credential path.
    if (!startup_preset_sent_ && ws_.is_open()) {
      startup_preset_sent_ = true;
      ws_.send_text(startup_preset_json_);
    }
    if (stream_decoding_ && !request_tiles_) {
      // Follow the host's global stream preference (VP9/AV1/tiles): the core
      // handles all of them, and the toolbar changes the codec deliberately.
      // Sending a codec here would silently reconfigure every other viewer.
      ws_.send_text("{\"type\":\"request-keyframe\"}");
      ws_.send_text("{\"type\":\"request-frame\"}");
    } else if (request_tiles_ && !tiles_requested_) {
      tiles_requested_ = true;
      // Do NOT push stream-codec=tiles here anymore. This hello already says
      // the viewer cannot decode any stream, and the supervisor only picks a
      // stream every viewer can decode, so the picture stays tiles anyway.
      // The old send was *host-global* and persisted: one native viewer left
      // the web client with "平铺" as the only codec in its menu.
      (void)has_preference;
      (void)preference;
      // The selected tier went out above; do not force BGRA with the retired
      // host-global `quality` message. `session-preset` is viewer-owned and
      // carries chroma, so truecolor/BGRA and turbo/gray are both expressible.
      // The host's push clock only sends on change; a fresh viewer on a
      // static screen would sit on "waiting for the first frame" forever.
      // The web asks for a keyframe and one frame as soon as it is live.
      ws_.send_text("{\"type\":\"request-keyframe\"}");
      ws_.send_text("{\"type\":\"request-frame\"}");
      // A full-screen tile refresh reads the current desktop pixels directly
      // and is not gated on the capture's stream/keyframe state. This is the
      // only reliable first picture when the host sits in a forced-stream
      // window (it answers keyframe requests with empty frames there).
      int screen_w = 0;
      int screen_h = 0;
      if (find_number(text, "screen_w", &screen_w) &&
          find_number(text, "screen_h", &screen_h) && screen_w > 0 &&
          screen_h > 0) {
        screen_w_ = screen_w;
        screen_h_ = screen_h;
        screen_width_.store(screen_w);
        screen_height_.store(screen_h);
        ws_.send_text(
            "{\"type\":\"request-tiles\",\"tiles\":[{\"x\":0,\"y\":0,"
            "\"w\":" + std::to_string(screen_w) + ",\"h\":" +
            std::to_string(screen_h) + "}]}");
      }
    } else if (!request_tiles_ && !stream_decoding_) {
      // Surface mode without stream decoding: the core paints whatever the
      // host sends, so a first keyframe/frame is still required. Without this
      // branch the session sat on "已连接 · 等待画面" forever while input
      // worked (the host's push clock has nothing to send on a still screen).
      ws_.send_text("{\"type\":\"request-keyframe\"}");
      ws_.send_text("{\"type\":\"request-frame\"}");
    }
    int width = 0;
    int height = 0;
    if (find_number(text, "screen_w", &width) &&
        find_number(text, "screen_h", &height) && width > 0) {
      stats_->resolution = std::to_wstring(width) + L"x" +
                           std::to_wstring(height);
      // The full-screen bootstrap requests need the size even in surface mode
      // (the tile branch above only runs for a direct tile client).
      screen_w_ = width;
      screen_h_ = height;
      screen_width_.store(width);
      screen_height_.store(height);
    }
    std::string name;
    if (find_string(text, "device_name", &name) && !name.empty()) {
      device_name_ = wide_of(name);
    }
    std::string security;
    if (find_string(text, "security", &security) && !security.empty()) {
      security_ = wide_of(security);
    }
    bool input_available = false;
    if (json_bool_field(text, "input", 0, &input_available)) {
      input_available_.store(input_available);
    }
    // Clipboard-file capabilities. `clipboard_stream` is the host confirming
    // it will fetch ranges from our lazy offer; `clipboard_pull` is the
    // reverse direction (`clipboard-sync.files` + 0x18). `nested_rdp` is only
    // a badge: the host streams the inner client's files either way.
    bool clipboard_flag = false;
    if (json_bool_field(text, "clipboard_stream", 0, &clipboard_flag)) {
      host_clipboard_stream_ = clipboard_flag;
    }
    if (json_bool_field(text, "clipboard_pull", 0, &clipboard_flag)) {
      host_clipboard_pull_ = clipboard_flag;
    }
    if (json_bool_field(text, "clipboard_nested_rdp", 0, &clipboard_flag)) {
      host_nested_rdp_ = clipboard_flag;
    }
    std::string version;
    if (json_string_field(text, "version", 0, &version) &&
        !version.empty()) {
      std::lock_guard<std::mutex> lock(log_mutex_);
      host_logs_.version = version;
      host_version_ = wide_of(version);
    }
    std::string tiles_backend;
    if (json_object_string_field(text, "tiles", "backend", &tiles_backend) &&
        !tiles_backend.empty()) {
      std::lock_guard<std::mutex> lock(log_mutex_);
      tiles_backend_ = wide_of(tiles_backend);
    }
    molink::native_diag::diag().log(
        molink::native_diag::level::info,
        molink::native_diag::component::session, "welcome", std::string(),
        {{"version", version},
         {"security", utf8_of(security_)},
         {"video_mode", utf8_of(video_mode_)}});
  } else if (type == "codec") {
    const uint64_t now = GetTickCount64();
    if (now - last_ping_ >= 1000) {
      int wire_rate = 0;
      if (find_number(text, "wire_rate", &wire_rate) && wire_rate > 0) {
        stats_->note_bitrate(wire_rate * 8.0 / 1000.0);
      }
      last_ping_ = now;
    }
  } else if (type == "transport") {
    std::string path;
    std::string protocol;
    if (find_string(text, "path", &path) && !path.empty()) {
      path_label_ = wide_of(path);
    }
    if (find_string(text, "protocol", &protocol) && !protocol.empty()) {
      protocol_label_ = wide_of(protocol);
    }
    molink::native_diag::diag().log(
        molink::native_diag::level::info,
        molink::native_diag::component::transport, "path", std::string(),
        {{"path", path}, {"protocol", protocol}});
  } else if (type == "access-requested") {
    access_pending_ = true;
  } else if (type == "hello_required") {
    // The first hello can race the relay/UDP path coming up. The web client
    // resends it; so do we, a few times.
    // Answer at once: the host sends hello_required when the first hello
    // raced the transport coming up, and it only sends it once. The old
    // 200 ms gate skipped this resend (the rejection always arrives faster)
    // and left the session waiting forever with no welcome.
    const uint64_t now = GetTickCount64();
    if (!hello_text_.empty() && hello_resends_ < 6) {
      ++hello_resends_;
      hello_sent_at_ = now;
      if (access_request_) access_pending_ = true;
      ws_.send_text(hello_text_);
    }
  } else if (type == "error") {
    std::string code;
    std::string message;
    find_string(text, "code", &code);
    find_string(text, "message", &message);
    std::string combined = code;
    if (!message.empty()) {
      if (!combined.empty()) combined += ": ";
      combined += message;
    }
    if (combined.empty()) combined = "host error";
    if (message.empty()) message = combined;
    // Keep the two channels apart: a feature failure must not overwrite the
    // credential error the banner is showing.
    const auto route = molink::native::alerts::classify(code, message);
    if (route.where == molink::native::alerts::surface::banner) {
      last_error_ = wide_of(combined);
      last_error_code_ = wide_of(code);
      error_seq_.fetch_add(1);
      access_pending_ = false;
    } else {
      last_feature_error_ = wide_of(combined);
      last_feature_error_code_ = wide_of(code);
      feature_error_seq_.fetch_add(1);
    }
    molink::native_diag::diag().log(
        molink::native_diag::level::warn, diag_component_for(code), "error",
        message, {{"err_code", code}});
  } else if (type == "logs") {
    // E2: the host's own diagnostic tail/info. The tail is UI state, not a
    // log line: writing it back into our copy would duplicate every entry.
    std::string action;
    json_string_field(text, "action", 0, &action);
    const uint64_t now = GetTickCount64();
    std::lock_guard<std::mutex> lock(log_mutex_);
    if (action == "tail") {
      std::string tail;
      if (json_string_field(text, "tail", 0, &tail)) {
        host_logs_.tail = std::move(tail);
        host_logs_.has_tail = true;
      }
      uint64_t number = 0;
      if (json_u64_field(text, "size", 0, &number)) {
        host_logs_.tail_size = number;
      }
      if (json_u64_field(text, "lines", 0, &number)) {
        host_logs_.tail_lines = static_cast<int>(number);
      }
      host_logs_.tail_at = now;
    } else if (action == "info") {
      bool flag = false;
      if (json_bool_field(text, "enabled", 0, &flag)) {
        host_logs_.enabled = flag;
      }
      std::string value;
      if (json_string_field(text, "directory", 0, &value)) {
        host_logs_.directory = std::move(value);
      }
      if (json_string_field(text, "file", 0, &value)) {
        host_logs_.file = std::move(value);
      }
      uint64_t number = 0;
      if (json_u64_field(text, "retention_days", 0, &number)) {
        host_logs_.retention_days = static_cast<int>(number);
      }
      if (json_u64_field(text, "current_bytes", 0, &number)) {
        host_logs_.current_bytes = number;
      }
      host_logs_.has_info = true;
      host_logs_.info_at = now;
    } else if (action == "state") {
      bool flag = false;
      if (json_bool_field(text, "enabled", 0, &flag)) {
        host_logs_.enabled = flag;
        host_logs_.has_info = true;
        host_logs_.info_at = now;
      }
    }
    molink::native_diag::diag().log(
        molink::native_diag::level::debug,
        molink::native_diag::component::session, "logs.reply", std::string(),
        {{"action", action}});
  } else if (type == "pong") {
    // The host echoes the ping's `t`, so the RTT needs no shared state.
    uint64_t sent = 0;
    const uint64_t now = now_us();
    const bool found = find_u64(text, "t", &sent);
    if (std::getenv("MOLINK_WS_LOG") != nullptr) {
      std::fprintf(stderr, "pong: found=%d sent=%llu now=%llu rtt=%lld\n",
                   found ? 1 : 0, static_cast<unsigned long long>(sent),
                   static_cast<unsigned long long>(now),
                   static_cast<long long>(now) - static_cast<long long>(sent));
    }
    if (found && sent > 0 && now >= sent) {
      last_rtt_ms_ = static_cast<int>((now - sent + 500) / 1000);
      stats_->note_latency(last_rtt_ms_);
    }
    ping_sent_at_.store(0);
  } else if (type == "chat") {
    std::string action;
    std::string message;
    if (find_string(text, "action", &action) && action == "message" &&
        find_string(text, "text", &message) && !message.empty()) {
      std::lock_guard<std::mutex> lock(inbox_mutex_);
      chat_inbox_.push_back(message);
    }
  } else if (type == "clipboard" || type == "clipboard-sync") {
    // The id-based pull list is a different message shape from the legacy
    // `clipboard action=files` list: only `clipboard-sync` carries session
    // ids, and only that list may be downloaded (the legacy one is for the
    // Web files panel / older clients).
    if (type == "clipboard-sync") {
      std::string clip_probe;
      if (text.find("\"files\"") != std::string::npos) {
        file_event event;
        event.action = L"clipboard-pull-offer";
        for (const std::string& item : json_object_array(text, "files")) {
          molink::clipboard_files::entry file;
          uint64_t number = 0;
          std::string value;
          if (json_u64_field(item, "id", 0, &number)) {
            file.id = static_cast<uint32_t>(number);
          }
          if (json_string_field(item, "name", 0, &value)) file.name = value;
          if (json_u64_field(item, "size", 0, &number)) file.size = number;
          if (json_string_field(item, "path", 0, &value)) file.path = value;
          event.clip_files.push_back(std::move(file));
        }
        uint64_t omitted = 0;
        if (json_u64_field(text, "omitted", 0, &omitted)) {
          event.clip_omitted = static_cast<std::size_t>(omitted);
        }
        bool nested = false;
        if (json_bool_field(text, "nested_rdp", 0, &nested)) {
          event.clip_nested_rdp = nested;
        }
        std::lock_guard<std::mutex> lock(file_mutex_);
        file_events_.push_back(std::move(event));
      } else if (find_string(text, "text", &clip_probe) && !clip_probe.empty()) {
        // Text replaced the files: any in-flight pull now addresses ids that
        // no longer exist. Abandon it before the next range comes back gone.
        file_event event;
        event.action = L"clipboard-pull-offer";
        std::lock_guard<std::mutex> lock(file_mutex_);
        file_events_.push_back(std::move(event));
      }
    }
    std::string action;
    std::string clip;
    const bool has_action = find_string(text, "action", &action);
    if (find_string(text, "text", &clip) && !clip.empty() &&
        (type == "clipboard-sync" || !has_action || action == "set" ||
         action == "sync")) {
      std::lock_guard<std::mutex> lock(inbox_mutex_);
      clipboard_inbox_.push_back(clip);
    }
  } else if (type == "file") {
    handle_file_json(text);
  } else if (type == "stream-codec") {
    std::string value;
    if (find_string(text, "value", &value) && !value.empty()) {
      stats_->codec = wide_of(value);
    }
  } else if (type == "video-mode") {
    // Host-global broadcast: another viewer (or this one) changed the mode.
    std::string value;
    if (find_string(text, "value", &value) && !value.empty()) {
      video_mode_ = wide_of(value);
    }
  } else if (type == "session-preset") {
    // The host's reply is an observation, never a command: record what it
    // actually applied and leave the viewer's selector alone (Web
    // `onSessionPresetApplied` does exactly this).
    preset::applied_reply reply;
    if (preset::parse_session_preset_reply(text, &reply) && reply.applied) {
      std::lock_guard<std::mutex> lock(applied_mutex_);
      applied_ = std::move(reply);
    }
  } else if (type == "quality") {
    // Retired host-global acknowledgement. Older cores still answer a
    // viewer-owned preset request this way; use it as a fallback so the
    // actual-values line is not stuck on "waiting".
    preset::applied_reply reply;
    if (preset::parse_quality_reply(text, &reply)) {
      std::lock_guard<std::mutex> lock(applied_mutex_);
      applied_ = std::move(reply);
    }
  }
}

void session_client::tick(uint64_t now_ms) {
  if (!ws_.is_open()) return;
  // One ping is in flight at a time. A pong that never arrives means the
  // link is gone even if the socket still looks open (the old code simply
  // stopped pinging and sat there until the host's idle timeout); close it
  // and let the shell's reconnect cycle take over, like the web's
  // missed-pong limit.
  if (ping_sent_at_ != 0 && now_ms - ping_sent_at_ > 6000) {
    ws_.close();
    return;
  }
  if (authenticated_ && !extra_frame_requested_ &&
      now_ms - authenticated_at_ >= 1000) {
    extra_frame_requested_ = true;
    ws_.send_text("{\"type\":\"request-frame\"}");
  }
  // Tile repairs come from the host's once-a-second tile clock (0x15), which
  // names exactly the cells this client missed. A full-screen refresh is only
  // a fallback for a host that stops sending clocks: once every five seconds,
  // instead of the old 700 ms poll that re-sent the whole BGRA picture on
  // every static desktop (megabytes each time, and enough backpressure to
  // drop the session on a WAN link).
  if (!stream_decoding_ && authenticated_ && has_image_.load() &&
      request_tiles_ && screen_w_ > 0 && screen_h_ > 0 &&
      now_ms - last_clock_seen_ >= 5000 &&
      now_ms - last_refresh_at_ >= 5000) {
    last_refresh_at_ = now_ms;
    ws_.send_text(
        "{\"type\":\"request-tiles\",\"tiles\":[{\"x\":0,\"y\":0,"
        "\"w\":" + std::to_string(screen_w_) + ",\"h\":" +
        std::to_string(screen_h_) + "}]}");
  }
  // A session without a picture keeps asking. The surface path is included:
  // the core decodes the stream or paints tiles and publishes the surface, so
  // its "picture exists" signal is note_surface_frame(), not a tile decode.
  // Stop after the cap; the periodic refresh/timeout path takes over.
  if (authenticated_ && !has_image_.load() && bootstrap_requests_ < 40 &&
      now_ms - last_bootstrap_at_ >= 500) {
    ++bootstrap_requests_;
    last_bootstrap_at_ = now_ms;
    if (screen_w_ > 0 && screen_h_ > 0 &&
        (request_tiles_ || stream_decoding_) && (bootstrap_requests_ % 4) == 0) {
      // Even a surface viewer benefits from a full-screen tile refresh: the
      // core composites it directly, and it is not gated on the host's
      // stream/keyframe state.
      ws_.send_text(
          "{\"type\":\"request-tiles\",\"tiles\":[{\"x\":0,\"y\":0,"
          "\"w\":" + std::to_string(screen_w_) + ",\"h\":" +
          std::to_string(screen_h_) + "}]}");
    } else {
      ws_.send_text("{\"type\":\"request-frame\"}");
    }
    if ((bootstrap_requests_ & 1) != 0) {
      ws_.send_text("{\"type\":\"request-keyframe\"}");
    }
  }
  // Once a second: real received bitrate for the watermark, and (in surface
  // mode) a viewer sample so the host's QoS can follow what this client can
  // actually take. A direct tile client never reports paint rates: the host
  // would read a slow tile painter as a starved link and switch to a stream
  // that client cannot decode.
  if (now_ms - stat_window_start_ >= 1000) {
    const uint64_t elapsed = now_ms - stat_window_start_;
    const double fps =
        stat_frames_ * 1000.0 / static_cast<double>(elapsed > 0 ? elapsed : 1);
    stat_frames_ = 0;
    const uint64_t bytes = stat_bytes_.exchange(0);
    const uint64_t rx = bytes * 1000 / (elapsed > 0 ? elapsed : 1000);
    stat_window_start_ = now_ms;
    if (stats_ != nullptr) {
      stats_->note_bitrate(static_cast<double>(rx) * 8.0 / 1000.0);
    }
    if (stream_decoding_) {
      char buffer[192];
      std::snprintf(buffer, sizeof(buffer),
                    "{\"type\":\"viewer\",\"fps\":%.1f,\"dropped\":0,"
                    "\"rx\":%llu,\"rtt\":%d}",
                    fps, static_cast<unsigned long long>(rx), last_rtt_ms_);
      ws_.send_text(buffer);
    }
  }
  if (now_ms - last_ping_ >= 1000 && ping_sent_at_ == 0) {
    const std::string ping =
        "{\"type\":\"ping\",\"t\":" + std::to_string(now_us()) + "}";
    if (ws_.send_text(ping)) ping_sent_at_ = now_ms;
  }
}

bool session_client::send_session_preset(const std::string& json) {
  if (!ws_.is_open()) return false;
  if (!ws_.send_text(json)) return false;
  // The Web requests a keyframe after a tier change (SessionView's
  // requestKeyframe); the host also forces one when it rebuilds the encoder,
  // but an explicit request covers a pure fps/knob change and a lost reply.
  if (authenticated_) ws_.send_text("{\"type\":\"request-keyframe\"}");
  return true;
}

preset::applied_reply session_client::applied_preset() const {
  std::lock_guard<std::mutex> lock(applied_mutex_);
  return applied_;
}

bool session_client::request_log_tail(int lines) {
  if (!ws_.is_open() || !authenticated_) return false;
  if (lines < 1) lines = 1;
  if (lines > 5000) lines = 5000;
  return ws_.send_text("{\"type\":\"logs\",\"action\":\"tail\",\"lines\":" +
                       std::to_string(lines) + "}");
}

bool session_client::request_log_info() {
  if (!ws_.is_open() || !authenticated_) return false;
  return ws_.send_text("{\"type\":\"logs\",\"action\":\"info\"}");
}

bool session_client::set_host_logging(bool enabled) {
  if (!ws_.is_open() || !authenticated_) return false;
  return ws_.send_text(std::string("{\"type\":\"logs\",\"action\":\"") +
                       (enabled ? "enable" : "disable") + "\"}");
}

host_log_state session_client::host_logs() const {
  std::lock_guard<std::mutex> lock(log_mutex_);
  return host_logs_;
}

std::wstring session_client::tiles_backend() const {
  std::lock_guard<std::mutex> lock(log_mutex_);
  return tiles_backend_;
}

std::wstring session_client::host_version() const {
  std::lock_guard<std::mutex> lock(log_mutex_);
  return host_version_;
}

bool session_client::send_json(const std::string& text) {
  return ws_.is_open() && ws_.send_text(text);
}

std::wstring session_client::live_codec_name() const {
  switch (live_codec_.load()) {
    case 12: return L"AV1";
    case 14: return L"H.264";
    case 15: return L"VP8";
    case 16: return L"VP9";
    case 0: return std::wstring();
    default: return L"TILES";
  }
}

bool session_client::send_video_mode(const std::string& mode) {
  if (!ws_.is_open()) return false;
  // Only the two deterministic paths remain; an old caller asking for the
  // removed adaptive mode is sent as tiles.
  const std::string value = mode == "stream" ? "stream" : "tiles";
  if (mode != "stream" && mode != "tiles" && mode != "mixed") return false;
  video_mode_ = wide_of(value);
  return ws_.send_text("{\"type\":\"video-mode\",\"value\":\"" + value + "\"}");
}

void session_client::stop() { ws_.close(); }

}  // namespace molink::net
