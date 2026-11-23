#include "preset_message.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace molink::preset {
namespace {

constexpr preset_plan kPlans[plan_count] = {
    {preset_id::turbo, "turbo", "极速",
     "弱网优先：低帧率、灰色画面、最低码率。", "turbo", 10, 5, 15, 0, 2500,
     "auto", "preset", "gray"},
    {preset_id::smooth, "smooth", "流畅",
     "默认：24 帧、16 位色、快速压缩，低带宽下也流畅。", "smooth", 24, 8, 30, 0,
     6000, "auto", "preset", "420"},
    {preset_id::balanced, "balanced", "标准",
     "30 帧、4:2:0 视频，压缩自适应，日常桌面最稳妥。", "balanced", 30, 10, 30,
     0, 0, "auto", "preset", "420"},
    {preset_id::hd, "hd", "高清",
     "画质优先：30–60 帧、高质量 4:2:0 视频，靠 QP 与码率提升画质。", "quality",
     45, 15, 60, 0, 0, "auto", "preset", "420"},
    {preset_id::truecolor, "truecolor", "真彩色",
     "真彩色（不压缩色度，使用平铺）：4:4:4 不抽样、最高质量，总走平铺而不是"
     "视频流，帧率降到 18 以保画质。",
     "lossless", 18, 10, 20, 0, 0, "cq", "bgra", "444"},
    {preset_id::auto_, "auto", "自动",
     "按延迟、带宽和画面变化自动选挡；只有选中它才允许自适应。", "balanced", 30,
     5, 30, 0, 0, "auto", "preset", "auto"},
};

bool equals(std::string_view value, const char* text) {
  return value == std::string_view(text);
}

int clamp_int(int value, int minimum, int maximum) {
  return std::max(minimum, std::min(maximum, value));
}

void append_json_string(std::string& out, std::string_view value) {
  out.push_back('"');
  for (const char raw : value) {
    const unsigned char c = static_cast<unsigned char>(raw);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
          out += buffer;
        } else {
          out.push_back(raw);
        }
        break;
    }
  }
  out.push_back('"');
}

void append_field(std::string& out, const char* name, std::string_view value,
                  bool first) {
  if (!first) out.push_back(',');
  append_json_string(out, name);
  out.push_back(':');
  append_json_string(out, value);
}

void append_field(std::string& out, const char* name, int value, bool first) {
  if (!first) out.push_back(',');
  append_json_string(out, name);
  out.push_back(':');
  out += std::to_string(value);
}

/// One field order for both fixed and custom messages, matching the Web
/// object literal (`type, preset, fps, quality, bitrate_kbps, rate_mode,
/// format, chroma, codec`).
std::string build_message(std::string_view preset, int fps, int quality,
                          int bitrate_kbps, std::string_view rate_mode,
                          std::string_view format, std::string_view chroma,
                          std::string_view codec) {
  std::string out = "{";
  append_field(out, "type", std::string_view("session-preset"), true);
  append_field(out, "preset", preset, false);
  append_field(out, "fps", fps, false);
  append_field(out, "quality", quality, false);
  append_field(out, "bitrate_kbps", bitrate_kbps, false);
  append_field(out, "rate_mode", rate_mode, false);
  append_field(out, "format", format, false);
  append_field(out, "chroma", chroma, false);
  append_field(out, "codec", codec, false);
  out.push_back('}');
  return out;
}

// ---------------------------------------------------------------------------
// Minimal JSON reader. The protocol only needs objects/arrays/scalars, but
// strings have to decode escapes so a host may echo anything safely.
// ---------------------------------------------------------------------------

struct json_value {
  enum class kind { null, boolean, number, string, object, array };

  kind type = kind::null;
  bool boolean = false;
  double number = 0;
  std::string string;
  std::map<std::string, json_value> object;
  std::vector<json_value> array;

  const json_value* find(std::string_view key) const {
    if (type != kind::object) return nullptr;
    const auto it = object.find(std::string(key));
    return it == object.end() ? nullptr : &it->second;
  }
};

void append_utf8(std::string& out, uint32_t cp) {
  if (cp <= 0x7f) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7ff) {
    out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  } else if (cp <= 0xffff) {
    out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  } else {
    out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  }
}

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

class json_parser {
 public:
  json_parser(const char* begin, const char* end) : p_(begin), end_(end) {}

  bool parse(json_value& out) {
    skip_space();
    if (!parse_value(out)) return false;
    skip_space();
    return p_ == end_;
  }

 private:
  const char* p_;
  const char* end_;

  void skip_space() {
    while (p_ != end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' ||
                           *p_ == '\r')) {
      ++p_;
    }
  }

  bool consume(char expected) {
    skip_space();
    if (p_ == end_ || *p_ != expected) return false;
    ++p_;
    return true;
  }

  bool parse_value(json_value& out) {
    skip_space();
    if (p_ == end_) return false;
    switch (*p_) {
      case '{':
        return parse_object(out);
      case '[':
        return parse_array(out);
      case '"':
        out.type = json_value::kind::string;
        return parse_string(out.string);
      case 't':
        if (end_ - p_ >= 4 && std::string_view(p_, 4) == "true") {
          p_ += 4;
          out.type = json_value::kind::boolean;
          out.boolean = true;
          return true;
        }
        return false;
      case 'f':
        if (end_ - p_ >= 5 && std::string_view(p_, 5) == "false") {
          p_ += 5;
          out.type = json_value::kind::boolean;
          out.boolean = false;
          return true;
        }
        return false;
      case 'n':
        if (end_ - p_ >= 4 && std::string_view(p_, 4) == "null") {
          p_ += 4;
          out.type = json_value::kind::null;
          return true;
        }
        return false;
      default:
        return parse_number(out);
    }
  }

  bool parse_object(json_value& out) {
    if (!consume('{')) return false;
    out.type = json_value::kind::object;
    skip_space();
    if (p_ != end_ && *p_ == '}') {
      ++p_;
      return true;
    }
    while (p_ != end_) {
      std::string key;
      if (!parse_string(key)) return false;
      if (!consume(':')) return false;
      json_value value;
      if (!parse_value(value)) return false;
      out.object[std::move(key)] = std::move(value);
      skip_space();
      if (p_ == end_) return false;
      if (*p_ == ',') {
        ++p_;
        continue;
      }
      if (*p_ == '}') {
        ++p_;
        return true;
      }
      return false;
    }
    return false;
  }

  bool parse_array(json_value& out) {
    if (!consume('[')) return false;
    out.type = json_value::kind::array;
    skip_space();
    if (p_ != end_ && *p_ == ']') {
      ++p_;
      return true;
    }
    while (p_ != end_) {
      json_value value;
      if (!parse_value(value)) return false;
      out.array.push_back(std::move(value));
      skip_space();
      if (p_ == end_) return false;
      if (*p_ == ',') {
        ++p_;
        continue;
      }
      if (*p_ == ']') {
        ++p_;
        return true;
      }
      return false;
    }
    return false;
  }

  bool parse_string(std::string& out) {
    skip_space();
    if (p_ == end_ || *p_ != '"') return false;
    ++p_;
    out.clear();
    while (p_ != end_) {
      const char c = *p_++;
      if (c == '"') return true;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (p_ == end_) return false;
      const char escape = *p_++;
      switch (escape) {
        case '"':
        case '\\':
        case '/':
          out.push_back(escape);
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u': {
          if (end_ - p_ < 4) return false;
          uint32_t cp = 0;
          for (int i = 0; i < 4; ++i) {
            const int digit = hex_digit(p_[i]);
            if (digit < 0) return false;
            cp = (cp << 4) | static_cast<uint32_t>(digit);
          }
          p_ += 4;
          if (cp >= 0xd800 && cp <= 0xdbff) {
            // Surrogate pair: combine with the low half when it is present.
            if (end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
              uint32_t low = 0;
              bool ok = true;
              for (int i = 0; i < 4; ++i) {
                const int digit = hex_digit(p_[2 + i]);
                if (digit < 0) {
                  ok = false;
                  break;
                }
                low = (low << 4) | static_cast<uint32_t>(digit);
              }
              if (ok && low >= 0xdc00 && low <= 0xdfff) {
                p_ += 6;
                cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
              }
            }
          }
          append_utf8(out, cp);
          break;
        }
        default:
          return false;
      }
    }
    return false;
  }

  bool parse_number(json_value& out) {
    skip_space();
    char* stop = nullptr;
    const double value = std::strtod(p_, &stop);
    if (stop == p_ || stop > end_) return false;
    p_ = stop;
    out.type = json_value::kind::number;
    out.number = value;
    return true;
  }
};

bool parse_json(std::string_view text, json_value& out) {
  // strtod() needs a terminated buffer; keep the copy local and let the
  // parser stop at end_.
  const std::string buffer(text);
  json_parser parser(buffer.data(), buffer.data() + buffer.size());
  return parser.parse(out);
}

std::string string_field(const json_value& object, const char* name) {
  const json_value* value = object.find(name);
  if (value == nullptr || value->type != json_value::kind::string) return {};
  return value->string;
}

int int_field(const json_value& object, const char* name, int fallback) {
  const json_value* value = object.find(name);
  if (value == nullptr || value->type != json_value::kind::number) {
    return fallback;
  }
  return static_cast<int>(value->number);
}

bool bool_field(const json_value& object, const char* name) {
  const json_value* value = object.find(name);
  return value != nullptr && value->type == json_value::kind::boolean &&
         value->boolean;
}

const char* chroma_for_tile_format(std::string_view format) {
  if (equals(format, "bgra") || equals(format, "rgb24")) return "444";
  if (equals(format, "rgb565")) return "565";
  if (equals(format, "gray8")) return "gray";
  return "";
}

preset_id nearest_legacy(std::string_view value) {
  // Old native tiers: quality ("清晰") maps to hd, the tier that keeps the
  // host's `quality` mode; ultra ("超清") and lossless ("无损") both kept the
  // original pixels, which is what truecolor asks for today.
  if (equals(value, "quality")) return preset_id::hd;
  if (equals(value, "ultra") || equals(value, "lossless") ||
      equals(value, "raw") || equals(value, "truecolor")) {
    return preset_id::truecolor;
  }
  if (equals(value, "game")) return preset_id::smooth;
  if (equals(value, "turbo")) return preset_id::turbo;
  if (equals(value, "smooth")) return preset_id::smooth;
  if (equals(value, "balanced")) return preset_id::balanced;
  if (equals(value, "hd")) return preset_id::hd;
  if (equals(value, "auto")) return preset_id::auto_;
  return default_preset();
}

}  // namespace

const preset_plan* plans() { return kPlans; }

const preset_plan& plan(preset_id id) {
  for (const preset_plan& item : kPlans) {
    if (item.id == id) return item;
  }
  return kPlans[1];  // smooth is the default
}

preset_id default_preset() { return preset_id::smooth; }

bool parse_preset_id(std::string_view value, preset_id* out) {
  for (const preset_plan& item : kPlans) {
    if (value == std::string_view(item.name)) {
      if (out != nullptr) *out = item.id;
      return true;
    }
  }
  return false;
}

preset_id migrate_preset_id(std::string_view value) {
  preset_id id = default_preset();
  if (parse_preset_id(value, &id)) return id;
  return nearest_legacy(value);
}

const char* preset_name(preset_id id) { return plan(id).name; }

advanced_params default_advanced_params(preset_id id) {
  const preset_plan& item = plan(id);
  advanced_params out;
  out.fps = item.fps;
  out.quality = item.qp;
  out.bitrate_kbps = item.bitrate_kbps;
  out.rate_mode = item.rate_mode;
  out.format = item.format;
  out.chroma = item.chroma;
  out.codec = "auto";
  return out;
}

bool is_explicit_stream_codec(std::string_view codec) {
  return equals(codec, "av1") || equals(codec, "h264") ||
         equals(codec, "vp9") || equals(codec, "vp8");
}

const char* format_for_chroma(std::string_view chroma,
                              std::string_view current_format) {
  if (equals(chroma, "444")) {
    return equals(current_format, "rgb24") ? "rgb24" : "bgra";
  }
  if (equals(chroma, "gray")) return "gray8";
  if (equals(chroma, "565")) return "rgb565";
  if (equals(chroma, "420")) {
    // A 4:2:0 request drops an explicit tile format back to the profile.
    return "preset";
  }
  // `auto` (and anything unknown) lets the host's selection stand: keep the
  // caller's explicit format, defaulting to the profile.
  if (equals(current_format, "rgb24")) return "rgb24";
  if (equals(current_format, "bgra")) return "bgra";
  if (equals(current_format, "gray8")) return "gray8";
  if (equals(current_format, "rgb565")) return "rgb565";
  return "preset";
}

std::string preset_message_json(preset_id id, int fps, std::string_view codec) {
  const preset_plan& item = plan(id);
  const int rate = clamp_int(fps == 0 ? item.fps : fps, 1, 60);
  return build_message(item.name, rate, item.qp, item.bitrate_kbps,
                       item.rate_mode, item.format, item.chroma, codec);
}

std::string advanced_preset_message_json(const advanced_params& params) {
  const int fps = clamp_int(params.fps, 5, 60);
  const int quality = clamp_int(params.quality, 0, 63);
  const int bitrate = clamp_int(params.bitrate_kbps, 0, 50000);
  const bool forced420 = equals(params.chroma, "444") &&
                         is_explicit_stream_codec(params.codec);
  const std::string_view chroma =
      forced420 ? std::string_view("420") : std::string_view(params.chroma);
  const char* format = format_for_chroma(chroma, params.format);
  return build_message("custom", fps, quality, bitrate, params.rate_mode,
                       format, chroma, params.codec);
}

bool parse_session_preset_reply(std::string_view json, applied_reply* out) {
  if (out == nullptr) return false;
  json_value root;
  if (!parse_json(json, root)) return false;
  if (root.type != json_value::kind::object) return false;
  if (string_field(root, "type") != "session-preset") return false;

  applied_reply reply;
  reply.applied = bool_field(root, "applied");
  reply.preset = string_field(root, "preset");
  const json_value* actual = root.find("actual");
  if (actual != nullptr && actual->type == json_value::kind::object) {
    reply.actual.fps = int_field(*actual, "fps", 0);
    reply.actual.quality = string_field(*actual, "quality");
    reply.actual.format = string_field(*actual, "format");
    reply.actual.chroma = string_field(*actual, "chroma");
    reply.actual.codec = string_field(*actual, "codec");
  }
  *out = std::move(reply);
  return true;
}

bool parse_quality_reply(std::string_view json, applied_reply* out) {
  if (out == nullptr) return false;
  json_value root;
  if (!parse_json(json, root)) return false;
  if (root.type != json_value::kind::object) return false;
  if (string_field(root, "type") != "quality") return false;

  applied_reply reply;
  reply.applied = true;
  reply.preset = string_field(root, "mode");
  reply.actual.fps = int_field(root, "fps", 0);
  reply.actual.quality = reply.preset;
  reply.actual.format = string_field(root, "format");
  reply.actual.chroma = chroma_for_tile_format(reply.actual.format);
  // The retired acknowledgement did not name the running encoder; leave the
  // codec empty rather than guess, and the panel shows "—".
  *out = std::move(reply);
  return true;
}

const char* chroma_label(std::string_view chroma) {
  if (equals(chroma, "444")) return "4:4:4";
  if (equals(chroma, "420")) return "4:2:0";
  if (equals(chroma, "gray") || equals(chroma, "gray8")) return "8-bit";
  if (equals(chroma, "565") || equals(chroma, "rgb565")) return "16-bit";
  return "—";
}

const char* format_label(std::string_view format) {
  if (equals(format, "bgra")) return "BGRA";
  if (equals(format, "rgb24")) return "RGB24";
  if (equals(format, "rgb565")) return "RGB565";
  if (equals(format, "gray8")) return "Gray8";
  return "—";
}

const char* codec_label(std::string_view codec) {
  if (equals(codec, "tiles")) return "平铺";
  if (equals(codec, "auto")) return "自动";
  if (equals(codec, "h264")) return "H.264";
  if (equals(codec, "av1")) return "AV1";
  if (equals(codec, "vp9")) return "VP9";
  if (equals(codec, "vp8")) return "VP8";
  return "—";
}

}  // namespace molink::preset
