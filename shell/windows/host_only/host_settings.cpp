#include "host_settings.h"

#include <cctype>

namespace molink {
namespace hostui {
namespace {

std::string trim(const std::string& text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end &&
         std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
    ++begin;
  }
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  return text.substr(begin, end - begin);
}

}  // namespace

bool parse_listen_port(const std::string& text, int& port) {
  const std::string value = trim(text);
  if (value.empty() || value.size() > 5) return false;
  int parsed = 0;
  for (char c : value) {
    if (c < '0' || c > '9') return false;
    parsed = parsed * 10 + (c - '0');
  }
  if (parsed < 1 || parsed > 65535) return false;
  port = parsed;
  return true;
}

std::string access_policy_from_switches(bool input_allowed,
                                        bool clipboard_allowed) {
  if (!input_allowed) return "view_only";
  return clipboard_allowed ? "full" : "assist";
}

access_switches switches_from_access_policy(const std::string& policy) {
  access_switches switches;
  if (policy == "view_only") {
    switches.input = false;
    switches.clipboard = false;
  } else if (policy == "assist") {
    switches.input = true;
    switches.clipboard = false;
  } else {
    switches.input = true;
    switches.clipboard = true;
  }
  return switches;
}

bool valid_long_term_code(const std::string& value, std::string& error) {
  if (value.size() < 8 || value.size() > 128) {
    error = "长期密码长度无效，允许范围 8-128";
    return false;
  }
  bool letter = false;
  bool digit = false;
  for (unsigned char c : value) {
    if (c <= 0x20 || c == 0x7F) {
      error = "长期密码不能包含空格或控制字符";
      return false;
    }
    letter = letter || std::isalpha(c) != 0;
    digit = digit || std::isdigit(c) != 0;
  }
  if (!letter || !digit) {
    error = "长期密码必须同时包含字母和数字";
    return false;
  }
  return true;
}

std::string url_encode(const std::string& value) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size() * 3);
  for (unsigned char c : value) {
    const bool unreserved =
        (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
    if (unreserved) {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 0x0F];
    }
  }
  return out;
}

std::string settings_query(bool allow_remote, bool discovery, bool host_only,
                           const std::string& access_policy) {
  std::string query = "allow_remote=";
  query += allow_remote ? "1" : "0";
  query += "&discovery=";
  query += discovery ? "1" : "0";
  query += "&host_only=";
  query += host_only ? "1" : "0";
  query += "&access_policy=";
  query += access_policy;
  return query;
}

std::string network_query(int tcp_port, int udp_port) {
  return "tcp=" + std::to_string(tcp_port) +
         "&udp=" + std::to_string(udp_port);
}

std::string auth_code_query(const std::string& mode, int uses,
                            const std::string& current_code) {
  std::string query = "mode=" + url_encode(mode);
  if (uses > 0) {
    query += "&uses=" + std::to_string(uses);
  }
  if (!current_code.empty()) {
    query += "&value=" + url_encode(current_code);
  }
  return query;
}

std::string long_term_query(const std::string& value) {
  return "value=" + url_encode(value);
}

}  // namespace hostui
}  // namespace molink
