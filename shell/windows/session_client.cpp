#include "session_client.h"

#include <cstdio>

namespace molink::net {
namespace {

std::string quality_path(const session_endpoint& endpoint) {
  if (!endpoint.target.empty()) {
    return "/ws/connect?target=" + endpoint.target;
  }
  return "/ws/direct?host=" + endpoint.host +
         "&port=" + std::to_string(endpoint.port);
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
  ws_.on_text = [this](const std::string& text) { handle_text(text); };
  ws_.on_close = [this] { if (stats_ != nullptr) stats_->codec = L"-"; };
  if (!ws_.connect(core_host, core_port, quality_path(endpoint), error)) {
    return false;
  }
  const std::string hello =
      "{\"type\":\"hello\",\"token\":\"" + endpoint.token +
      "\",\"seq_clock\":true,\"video_push\":true,\"h264_stream\":true,"
      "\"vp9_stream\":true,\"vp8_stream\":true,\"av1_native\":true}";
  return ws_.send_text(hello);
}

void session_client::handle_text(const std::string& text) {
  if (stats_ == nullptr) return;
  std::string type;
  if (!find_string(text, "type", &type)) return;
  if (type == "welcome") {
    int width = 0;
    int height = 0;
    if (find_number(text, "screen_w", &width) &&
        find_number(text, "screen_h", &height) && width > 0) {
      stats_->resolution = std::to_wstring(width) + L"x" +
                           std::to_wstring(height);
    }
    std::string preference;
    if (find_string(text, "codec_preference", &preference) &&
        !preference.empty()) {
      stats_->codec = std::wstring(preference.begin(), preference.end());
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
  } else if (type == "pong") {
    if (ping_sent_at_ != 0) {
      stats_->note_latency(static_cast<int>(GetTickCount64() - ping_sent_at_));
      ping_sent_at_ = 0;
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
  if (now_ms - last_ping_ >= 1000 && ping_sent_at_ == 0) {
    if (ws_.send_text("{\"type\":\"ping\"}")) ping_sent_at_ = now_ms;
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

void session_client::stop() { ws_.close(); }

}  // namespace molink::net
