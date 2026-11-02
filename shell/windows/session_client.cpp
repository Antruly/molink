#include "session_client.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
  const size_t at = json.find(needle, start);
  if (at == std::string::npos) return false;
  size_t pos = at + needle.size();
  skip_ws(json, &pos);
  if (pos >= json.size() || json[pos] != ':') return false;
  ++pos;
  skip_ws(json, &pos);
  if (!parse_json_string(json, &pos, out)) return false;
  if (after != nullptr) *after = pos;
  return true;
}

bool json_u64_field(const std::string& json, const char* key, size_t start,
                    uint64_t* out) {
  const std::string needle = std::string("\"") + key + "\"";
  const size_t at = json.find(needle, start);
  if (at == std::string::npos) return false;
  size_t pos = at + needle.size();
  skip_ws(json, &pos);
  if (pos >= json.size() || json[pos] != ':') return false;
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

bool json_bool_field(const std::string& json, const char* key, size_t start,
                     bool* out) {
  const std::string needle = std::string("\"") + key + "\"";
  const size_t at = json.find(needle, start);
  if (at == std::string::npos) return false;
  size_t pos = at + needle.size();
  skip_ws(json, &pos);
  if (pos >= json.size() || json[pos] != ':') return false;
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
  video_mode_.clear();
  avatar_.clear();
  close_detail_.clear();
  device_name_.clear();
  security_.clear();
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
    } else {
      return;
    }
    last_error_code_ = std::wstring(code.begin(), code.end());
    last_error_ = wide_of(code + ": " + reason);
    access_pending_ = false;
  };
  if (!ws_.connect(core_host, core_port, quality_path(endpoint), error)) {
    return false;
  }
  hello_resends_ = 0;
  access_request_ = endpoint.token.empty();
  if (access_request_) {
    // No code: the host shows an approval dialog to the person at the
    // machine. The session stays unauthenticated until they accept.
    access_pending_ = true;
    hello_text_ =
        "{\"type\":\"request-access\",\"name\":\"moLink native\","
        "\"client\":\"molink-native\"}";
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
        endpoint.token +
        "\",\"seq_clock\":true,\"video_push\":true,\"h264_stream\":false,"
        "\"vp9_stream\":" + stream_cap + ",\"vp8_stream\":" + stream_cap +
        ",\"av1_native\":" + stream_cap + "}";
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
  bool flag = true;
  if (json_bool_field(text, "ok", 0, &flag)) event.ok = flag;
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
    // entries: [{name, dir, size, mtime}, ...] — scan objects without
    // trusting quotes inside names (walk strings, not raw find).
    const size_t key = text.find("\"entries\"");
    const size_t array = key == std::string::npos
                             ? std::string::npos
                             : text.find('[', key);
    if (array != std::string::npos) {
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
        const std::string item = text.substr(begin, pos - begin);
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
        if (!entry.name.empty()) event.entries.push_back(std::move(entry));
      }
    }
  }
  std::lock_guard<std::mutex> lock(file_mutex_);
  file_events_.push_back(std::move(event));
}

void session_client::send_file_action(const std::string& action, uint32_t id,
                                      const std::string& extra) {
  std::string text = "{\"type\":\"file\",\"action\":\"" + action + "\"";
  if (id != 0) text += ",\"id\":" + std::to_string(id);
  text += extra;
  text += "}";
  ws_.send_text(text);
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
                                const std::wstring& destination) {
  if (!ws_.is_open()) return false;
  WIN32_FILE_ATTRIBUTE_DATA info{};
  if (!GetFileAttributesExW(local_path.c_str(), GetFileExInfoStandard, &info)) {
    return false;
  }
  upload_state upload;
  upload.key = next_upload_key_++;
  upload.name = name.empty() ? local_path : name;
  upload.path = local_path;
  upload.dest = destination;
  upload.size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) |
                static_cast<uint64_t>(info.nFileSizeLow);
  upload.state = 0;
  uploads_.emplace(upload.key, std::move(upload));
  return true;
}

void session_client::file_browse(const std::wstring& path) {
  send_file_action("browse", 0,
                   ",\"path\":\"" + json_escape(utf8_of(path)) + "\"");
}

void session_client::file_request(const std::wstring& path) {
  send_file_action("request", 0,
                   ",\"path\":\"" + json_escape(utf8_of(path)) + "\"");
}

void session_client::file_cancel(uint32_t id) {
  if (id == 0) {
    for (auto& [key, upload] : uploads_) {
      if (upload.state == 0 || upload.state == 1) {
        if (upload.host_id != 0) {
          send_file_action("cancel", upload.host_id, "");
        }
        upload.in.close();
        upload.state = 3;
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
      upload.state = 3;
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
    it->second.state = 3;
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
  file_event done;
  done.action = L"downloaded";
  done.id = download.id;
  done.size = download.size;
  done.name = download.name;
  done.path = download.path;
  shell_file_events_.push_back(std::move(done));
}

void session_client::pump_files() {
  // 1. Offer the next queued file when nothing is in flight.
  if (active_offer_key_ == 0) {
    for (auto& [key, upload] : uploads_) {
      if (upload.state == 0 && !upload.offer_sent) {
        upload.offer_sent = true;
        active_offer_key_ = key;
        send_file_action(
            "offer", 0,
            ",\"name\":\"" + json_escape(utf8_of(upload.name)) +
                "\",\"size\":" + std::to_string(upload.size) +
                ",\"dest\":\"" + json_escape(utf8_of(upload.dest)) + "\"");
        break;
      }
    }
  }

  // 2. Drain host messages.
  std::vector<file_event> incoming;
  {
    std::lock_guard<std::mutex> lock(file_mutex_);
    incoming.swap(file_events_);
  }
  for (file_event& event : incoming) {
    const std::wstring& action = event.action;
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
      if (download.state != 1 || event.offset != download.received) {
        download.out.close();
        if (!download.path.empty()) DeleteFileW(download.path.c_str());
        download.state = 3;
        continue;
      }
      download.out.write(
          reinterpret_cast<const char*>(event.chunk_data.data()),
          static_cast<std::streamsize>(event.chunk_data.size()));
      download.received += event.chunk_data.size();
      if (!download.out) {
        download.out.close();
        DeleteFileW(download.path.c_str());
        download.state = 3;
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
          upload.state = 3;
          upload.message = L"无法读取本地文件";
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
        if (event.name.empty()) event.name = target->name;
        if (action == L"done" && event.ok) {
          target->state = 2;
          target->saved_path = event.path;
        } else {
          target->state = 3;
          target->message = event.message;
          if (target->message.empty()) target->message = L"传输失败";
        }
        if (uploads_.find(active_offer_key_) != uploads_.end()) {
          active_offer_key_ = 0;
        }
      }
      shell_file_events_.push_back(std::move(event));
      continue;
    }
    if (action == L"sending") {
      if (std::getenv("MOLINK_WS_LOG") != nullptr) {
        std::fprintf(stderr, "file rx sending id=%u size=%llu\n", event.id,
                     static_cast<unsigned long long>(event.size));
      }
      download_state download;
      download.id = event.id;
      download.name = event.name.empty() ? L"file" : event.name;
      download.size = event.size;
      download.state = 1;
      if (download_dir_.empty()) {
        wchar_t profile[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) > 0) {
          download_dir_ = std::wstring(profile) + L"\\Downloads\\moLink";
        }
      }
      CreateDirectoryW(download_dir_.c_str(), nullptr);
      download.path = unique_download_path(download_dir_, download.name);
      download.out.open(download.path.c_str(),
                        std::ios::binary | std::ios::trunc);
      if (!download.out) {
        file_event failed;
        failed.action = L"error";
        failed.id = download.id;
        failed.name = download.name;
        failed.message = L"无法创建本地文件";
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
      if (it != downloads_.end()) {
        download_state& download = it->second;
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

  // A missing final chunk after `eof` must not leave the row spinning forever.
  const uint64_t now_ms = GetTickCount64();
  for (auto& [download_id, download] : downloads_) {
    if (download.state != 1 || !download.eof_seen ||
        now_ms - download.eof_at <= 3000) {
      continue;
    }
    download.out.close();
    DeleteFileW(download.path.c_str());
    download.state = 3;
    file_event failed;
    failed.action = L"error";
    failed.id = download.id;
    failed.name = download.name;
    failed.message = L"文件接收不完整";
    shell_file_events_.push_back(std::move(failed));
  }

  // 3. Send the active upload in bounded slices (UI thread, no worker).
  auto active = uploads_.find(active_offer_key_);
  if (active == uploads_.end() || active->second.state != 1) return;
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
    row.name = upload.name;
    row.size = upload.size;
    row.sent = upload.sent;
    row.download = false;
    row.state = upload.state;
    out.push_back(std::move(row));
  }
  for (const auto& [id, download] : downloads_) {
    file_transfer_state row;
    row.id = download.id;
    row.name = download.name;
    row.size = download.size;
    row.sent = download.received;
    row.download = true;
    row.state = download.state;
    out.push_back(std::move(row));
  }
  return out;
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
    std::string avatar;
    if (find_string(text, "avatar", &avatar) && !avatar.empty()) {
      avatar_ = wide_of(avatar);
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
      ws_.send_text(
          "{\"type\":\"quality\",\"mode\":\"balanced\",\"format\":\"bgra\"}");
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
    last_error_ = wide_of(combined);
    last_error_code_ = wide_of(code);
    access_pending_ = false;
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
  } else if (type == "quality") {
    std::string mode;
    if (find_string(text, "mode", &mode) && !mode.empty()) {
      // The toolbar shows the preset name; map it to the chip text.
      stats_->codec = stats_->codec;  // placeholder, preset is a UI field
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

void session_client::send_quality(const std::string& mode, int fps, int qp,
                                  int bitrate_kbps, const std::string& rate_mode,
                                  const std::string& format) {
  if (!ws_.is_open()) return;
  std::string text = "{\"type\":\"quality\",\"mode\":\"" + mode + "\"";
  if (fps > 0) text += ",\"fps\":" + std::to_string(fps);
  if (qp >= 0) text += ",\"qp\":" + std::to_string(qp);
  if (bitrate_kbps >= 0) text += ",\"bitrate_kbps\":" + std::to_string(bitrate_kbps);
  if (!rate_mode.empty()) text += ",\"rate_mode\":\"" + rate_mode + "\"";
  if (!format.empty()) text += ",\"format\":\"" + format + "\"";
  text += "}";
  ws_.send_text(text);
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
  if (mode != "stream" && mode != "mixed" && mode != "tiles") return false;
  video_mode_ = wide_of(mode);
  return ws_.send_text("{\"type\":\"video-mode\",\"value\":\"" + mode + "\"}");
}

void session_client::stop() { ws_.close(); }

}  // namespace molink::net
