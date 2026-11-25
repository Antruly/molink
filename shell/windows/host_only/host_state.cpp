#include "host_state.h"

#include <cctype>
#include <cstdlib>
#include <string>

namespace molink {
namespace hostui {
namespace {

bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

void skip_space(const std::string& json, std::size_t& pos) {
  while (pos < json.size() && is_space(json[pos])) ++pos;
}

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void append_utf8(std::string& out, unsigned codepoint) {
  if (codepoint <= 0x7F) {
    out += static_cast<char>(codepoint);
  } else if (codepoint <= 0x7FF) {
    out += static_cast<char>(0xC0 | (codepoint >> 6));
    out += static_cast<char>(0x80 | (codepoint & 0x3F));
  } else if (codepoint <= 0xFFFF) {
    out += static_cast<char>(0xE0 | (codepoint >> 12));
    out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (codepoint & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (codepoint >> 18));
    out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (codepoint & 0x3F));
  }
}

/// Parse a JSON string starting at @p pos (which must point at the opening
/// quote). Handles the escapes the core can emit; unknown escapes are kept
/// verbatim.
bool parse_string(const std::string& json, std::size_t& pos,
                  std::string& out) {
  if (pos >= json.size() || json[pos] != '"') return false;
  ++pos;
  while (pos < json.size()) {
    const char c = json[pos++];
    if (c == '"') return true;
    if (c != '\\') {
      out += c;
      continue;
    }
    if (pos >= json.size()) return false;
    const char escape = json[pos++];
    switch (escape) {
      case '"': out += '"'; break;
      case '\\': out += '\\'; break;
      case '/': out += '/'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'u': {
        if (pos + 4 > json.size()) return false;
        unsigned code = 0;
        for (int i = 0; i < 4; ++i) {
          const int digit = hex_digit(json[pos + i]);
          if (digit < 0) return false;
          code = (code << 4) | static_cast<unsigned>(digit);
        }
        pos += 4;
        // A surrogate pair needs its low half to become one codepoint.
        if (code >= 0xD800 && code <= 0xDBFF && pos + 6 <= json.size() &&
            json[pos] == '\\' && json[pos + 1] == 'u') {
          unsigned low = 0;
          bool ok = true;
          for (int i = 0; i < 4; ++i) {
            const int digit = hex_digit(json[pos + 2 + i]);
            if (digit < 0) {
              ok = false;
              break;
            }
            low = (low << 4) | static_cast<unsigned>(digit);
          }
          if (ok && low >= 0xDC00 && low <= 0xDFFF) {
            pos += 6;
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
          }
        }
        append_utf8(out, code);
        break;
      }
      default: out += escape; break;
    }
  }
  return false;
}

/// Walk the document and return the value position of the first @p key.
/// The scanner understands strings, so a device name that happens to contain
/// `"mode":` cannot be mistaken for a real field.
bool find_key(const std::string& json, const char* key, std::size_t& value_pos) {
  const std::string wanted(key);
  std::size_t pos = 0;
  while (pos < json.size()) {
    skip_space(json, pos);
    if (pos >= json.size()) break;
    const char c = json[pos];
    if (c == '"') {
      std::string name;
      const std::size_t after_quote = pos;
      if (!parse_string(json, pos, name)) return false;
      std::size_t probe = pos;
      skip_space(json, probe);
      if (probe < json.size() && json[probe] == ':') {
        if (name == wanted) {
          value_pos = probe + 1;
          return true;
        }
        pos = probe + 1;
        continue;
      }
      // It was a string value, not a key. Continue after it.
      pos = after_quote;
      parse_string(json, pos, name);
      continue;
    }
    if (c == '{' || c == '}' || c == '[' || c == ']' || c == ',' || c == ':') {
      ++pos;
      continue;
    }
    // Number / true / false / null: skip to the next structural character.
    while (pos < json.size() && json[pos] != ',' && json[pos] != '}' &&
           json[pos] != ']' && !is_space(json[pos])) {
      ++pos;
    }
  }
  return false;
}

bool value_string(const std::string& json, const char* key, std::string& out) {
  std::size_t pos = 0;
  if (!find_key(json, key, pos)) return false;
  skip_space(json, pos);
  out.clear();
  return parse_string(json, pos, out);
}

bool value_int(const std::string& json, const char* key, long& out) {
  std::size_t pos = 0;
  if (!find_key(json, key, pos)) return false;
  skip_space(json, pos);
  bool negative = false;
  if (pos < json.size() && json[pos] == '-') {
    negative = true;
    ++pos;
  }
  if (pos >= json.size() || !std::isdigit(static_cast<unsigned char>(json[pos]))) {
    return false;
  }
  long value = 0;
  while (pos < json.size() &&
         std::isdigit(static_cast<unsigned char>(json[pos]))) {
    value = value * 10 + (json[pos] - '0');
    ++pos;
  }
  out = negative ? -value : value;
  return true;
}

bool value_bool(const std::string& json, const char* key, bool& out) {
  std::size_t pos = 0;
  if (!find_key(json, key, pos)) return false;
  skip_space(json, pos);
  if (json.compare(pos, 4, "true") == 0) {
    out = true;
    return true;
  }
  if (json.compare(pos, 5, "false") == 0) {
    out = false;
    return true;
  }
  // Some producers use 0/1.
  long number = 0;
  if (value_int(json, key, number)) {
    out = number != 0;
    return true;
  }
  return false;
}

bool value_string_array(const std::string& json, const char* key,
                        std::vector<std::string>& out) {
  std::size_t pos = 0;
  if (!find_key(json, key, pos)) return false;
  skip_space(json, pos);
  if (pos >= json.size() || json[pos] != '[') return false;
  out.clear();
  ++pos;
  for (;;) {
    skip_space(json, pos);
    if (pos >= json.size()) return false;
    if (json[pos] == ']') {
      ++pos;
      return true;
    }
    if (json[pos] == ',') {
      ++pos;
      continue;
    }
    std::string item;
    if (!parse_string(json, pos, item)) return false;
    out.push_back(std::move(item));
  }
}

}  // namespace

bool parse_device(const std::string& body, device_info& out,
                  std::string& error) {
  out = device_info{};
  if (!value_string(body, "device_id", out.device_id) ||
      out.device_id.empty()) {
    error = "设备信息缺少 device_id";
    return false;
  }
  long port = 0;
  if (!value_int(body, "port", port) || port <= 0) {
    error = "设备信息缺少监听端口";
    return false;
  }
  out.port = static_cast<int>(port);
  long pid = 0;
  if (value_int(body, "pid", pid)) out.pid = static_cast<int>(pid);
  value_string(body, "device_name", out.device_name);
  value_string(body, "version", out.version);
  value_string(body, "platform", out.platform);
  value_string(body, "host", out.host);
  value_string(body, "token", out.token);
  value_string(body, "client_token", out.client_token);
  value_bool(body, "tls", out.tls);
  value_string(body, "value", out.code);
  value_string(body, "mode", out.code_mode);
  if (out.code_mode.empty()) out.code_mode = "single";
  long number = 0;
  if (value_int(body, "remaining", number)) {
    out.code_remaining = static_cast<int>(number);
  }
  if (value_int(body, "uses", number)) out.code_uses = static_cast<int>(number);
  value_bool(body, "long_term_set", out.long_term_set);
  value_bool(body, "single_enabled", out.single_enabled);
  value_bool(body, "multi_enabled", out.multi_enabled);
  value_string_array(body, "addresses", out.addresses);
  value_bool(body, "screen_capture", out.screen_capture);
  value_bool(body, "input_injection", out.input_injection);
  value_bool(body, "clipboard", out.clipboard);
  out.valid = true;
  return true;
}

bool parse_state(const std::string& body, state_info& out,
                 std::string& error) {
  out = state_info{};
  long number = 0;
  if (value_int(body, "port", number) && number > 0) {
    out.port = static_cast<int>(number);
  }
  if (value_int(body, "bound_port", number) && number > 0) {
    out.bound_port = static_cast<int>(number);
  }
  if (value_int(body, "tcp_port", number) && number > 0) {
    out.tcp_port = static_cast<int>(number);
  }
  if (value_int(body, "udp_port", number) && number > 0) {
    out.udp_port = static_cast<int>(number);
  }
  value_string(body, "device_id", out.device_id);
  value_string(body, "version", out.version);
  value_string(body, "host", out.host);
  value_bool(body, "restart_required", out.restart_required);
  value_bool(body, "allow_remote_access", out.allow_remote_access);
  value_bool(body, "host_only", out.host_only);
  value_bool(body, "enabled", out.server_enabled);
  value_bool(body, "online", out.server_online);
  value_string(body, "domain", out.server_domain);
  value_string(body, "display_id", out.server_display_id);
  value_string(body, "last_error", out.last_error);
  if (out.device_id.empty() && out.port <= 0) {
    error = "状态信息为空";
    return false;
  }
  out.valid = true;
  return true;
}

bool parse_settings(const std::string& body, settings_info& out,
                    std::string& error) {
  out = settings_info{};
  bool found = false;
  bool flag = false;
  if (value_bool(body, "allow_remote_access", flag)) {
    out.allow_remote_access = flag;
    found = true;
  }
  if (value_bool(body, "discovery_enabled", flag)) {
    out.discovery_enabled = flag;
    found = true;
  }
  if (value_bool(body, "host_only", flag)) {
    out.host_only = flag;
    found = true;
  }
  std::string policy;
  if (value_string(body, "access_policy", policy)) {
    if (policy == "full" || policy == "view_only" || policy == "assist") {
      out.access_policy = policy;
    }
    found = true;
  }
  std::string long_term;
  if (value_string(body, "long_term_code", long_term)) {
    out.long_term_set = !long_term.empty();
    found = true;
  }
  if (!found) {
    error = "设置信息为空";
    return false;
  }
  out.valid = true;
  return true;
}

std::string join_addresses(const device_info& device, bool allow_remote,
                           int port, std::size_t max_hosts) {
  if (port <= 0) port = device.port;
  const std::string suffix = ":" + std::to_string(port);
  if (!allow_remote) return "127.0.0.1" + suffix + "（仅本机）";
  if (device.addresses.empty()) return "127.0.0.1" + suffix;
  std::string text;
  std::size_t count = 0;
  for (const std::string& address : device.addresses) {
    if (count >= max_hosts) {
      text += " 等";
      break;
    }
    if (!text.empty()) text += ", ";
    text += address + suffix;
    ++count;
  }
  return text;
}

std::string code_summary(const device_info& device) {
  if (device.code.empty()) return "--";
  std::string text = device.code;
  if (device.code_mode == "multi") {
    text += "（多次";
    if (device.code_remaining > 0) {
      text += "，剩余 " + std::to_string(device.code_remaining) + " 次";
    } else {
      text += "，已用尽";
    }
    text += "）";
  } else {
    text += "（单次）";
  }
  return text;
}

std::string online_text(bool core_online) {
  return core_online ? "在线" : "离线";
}

bool effective_remote_access(const settings_info& settings,
                             const state_info& state) {
  if (settings.valid) return settings.allow_remote_access;
  return state.allow_remote_access;
}

std::string access_policy_text(const std::string& policy) {
  if (policy == "view_only") return "仅查看";
  if (policy == "assist") return "仅协助（可输入）";
  return "完整控制";
}

std::string format_version(const std::string& version) {
  if (version.empty()) return "版本 --";
  return "版本 " + version;
}

}  // namespace hostui
}  // namespace molink
