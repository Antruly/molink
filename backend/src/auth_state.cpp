#include "auth_state.h"

#include <algorithm>
#include <cctype>
#include <random>
#include <string>

#include "config.h"

namespace molink {
namespace {

constexpr int kRemoteSessionLimit = 64;

bool has_whitespace_or_control(const std::string& value) {
  for (unsigned char c : value) {
    if (c <= 0x20 || c == 0x7f) return true;
  }
  return false;
}

std::string generate_numeric(int digits) {
  static const char kDigits[] = "0123456789";
  std::random_device rd;
  std::string out;
  out.reserve(static_cast<std::size_t>(digits));
  for (int i = 0; i < digits; ++i) {
    out.push_back(kDigits[rd() % 10]);
  }
  return out;
}

std::string generate_long_term() {
  static const char kAlphabet[] =
      "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
  static constexpr std::size_t kSize = sizeof(kAlphabet) - 1;
  std::random_device rd;
  std::string out;
  out.reserve(24);
  bool letter = false;
  bool digit = false;
  for (int i = 0; i < 24; ++i) {
    const char c = kAlphabet[rd() % kSize];
    letter = letter || (c >= 'A' && c <= 'z');
    digit = digit || (c >= '0' && c <= '9');
    out.push_back(c);
  }
  // The alphabet mixes letters and digits; make the guarantee explicit.
  if (!letter) out[0] = 'm';
  if (!digit) out[1] = '7';
  return out;
}

bool valid_long_term(const std::string& value, std::string& err) {
  if (value.size() < 8 || value.size() > 128) {
    err = "long-term code must be 8..128 characters";
    return false;
  }
  if (has_whitespace_or_control(value)) {
    err = "long-term code must not contain spaces or control characters";
    return false;
  }
  bool letter = false;
  bool digit = false;
  for (unsigned char c : value) {
    letter = letter || std::isalpha(c) != 0;
    digit = digit || std::isdigit(c) != 0;
  }
  if (!letter || !digit) {
    err = "long-term code must contain both letters and digits";
    return false;
  }
  return true;
}

bool valid_temp_code(const std::string& value, std::string& err) {
  if (value.size() < 4 || value.size() > 64) {
    err = "temporary code must be 4..64 characters";
    return false;
  }
  if (has_whitespace_or_control(value)) {
    err = "temporary code must not contain spaces or control characters";
    return false;
  }
  return true;
}

}  // namespace

auth_state::auth_state(app_settings settings)
    : settings_(std::move(settings)), local_session_(generate_token()) {
  if (settings_.device_name.empty()) settings_.device_name = default_device_name();
  if (settings_.temp_mode != "single" && settings_.temp_mode != "multi") {
    settings_.temp_mode = "single";
  }
  if (settings_.temp_uses < 1) settings_.temp_uses = 1;
  if (settings_.temp_uses > 10000) settings_.temp_uses = 10000;

  std::string err;
  if (!valid_long_term(settings_.long_term_code, err)) {
    settings_.long_term_code = generate_long_term();
  }
  if (settings_.temp_code.empty() || settings_.temp_remaining <= 0 ||
      !valid_temp_code(settings_.temp_code, err)) {
    settings_.temp_remaining = settings_.temp_uses;
    settings_.temp_code = generate_numeric(6);
  }
  save_settings(settings_);
}

bool auth_state::authenticate(const std::string& supplied, bool consume) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (valid_session_locked(supplied)) return true;
  return verify_code_locked(supplied, consume);
}

bool auth_state::valid_session(const std::string& token) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return valid_session_locked(token);
}

bool auth_state::valid_session_locked(const std::string& token) const {
  if (token.empty()) return false;
  unsigned diff = constant_time_equals(token, local_session_) ? 0u : 1u;
  for (const std::string& session : remote_sessions_) {
    if (constant_time_equals(token, session)) diff = 0;
  }
  return diff == 0;
}

std::string auth_state::create_session() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string token = generate_token();
  remote_sessions_.push_back(token);
  while (remote_sessions_.size() > kRemoteSessionLimit) {
    remote_sessions_.pop_front();
  }
  return token;
}

code_view auth_state::view() const {
  std::lock_guard<std::mutex> lock(mutex_);
  code_view out;
  out.mode = settings_.temp_mode;
  out.value = settings_.temp_code;
  out.remaining = settings_.temp_remaining;
  out.uses = settings_.temp_uses;
  out.long_term_set = !settings_.long_term_code.empty();
  return out;
}

app_settings auth_state::settings() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return settings_;
}

std::string auth_state::set_long_term(const std::string& value,
                                      std::string& err) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string next = value;
  if (next.empty()) next = generate_long_term();
  if (!valid_long_term(next, err)) return std::string();
  settings_.long_term_code = next;
  // A new long-term code invalidates remote sessions, but not the local shell.
  invalidate_remote_sessions_locked();
  persist_locked();
  return next;
}

code_view auth_state::set_temp(const std::string& mode, int uses,
                               const std::string& value, std::string& err) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (mode != "single" && mode != "multi") {
    err = "mode must be single or multi";
    return code_view{};
  }
  int budget = mode == "single" ? 1 : uses;
  if (budget < 1) budget = 20;
  if (budget > 10000) budget = 10000;
  std::string next = value;
  if (next.empty()) next = generate_numeric(6);
  if (!valid_temp_code(next, err)) return code_view{};

  settings_.temp_mode = mode;
  settings_.temp_uses = budget;
  settings_.temp_remaining = budget;
  settings_.temp_code = next;
  persist_locked();

  code_view out;
  out.mode = settings_.temp_mode;
  out.value = settings_.temp_code;
  out.remaining = settings_.temp_remaining;
  out.uses = settings_.temp_uses;
  out.long_term_set = !settings_.long_term_code.empty();
  return out;
}

void auth_state::set_allow_access_requests(bool allow) {
  std::lock_guard<std::mutex> lock(mutex_);
  settings_.allow_access_requests = allow;
  persist_locked();
}

void auth_state::set_service_desired_running(bool enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  settings_.service_desired_running = enabled;
  persist_locked();
}

bool auth_state::set_server(std::string url, std::string invite,
                            std::string ca, bool enabled, std::string& err) {
  auto trim = [](std::string value) {
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t' ||
            value.back() == '\r' || value.back() == '\n')) {
      value.pop_back();
    }
    std::size_t start = 0;
    while (start < value.size() &&
           (value[start] == ' ' || value[start] == '\t')) {
      ++start;
    }
    return value.substr(start);
  };
  url = trim(std::move(url));
  invite = trim(std::move(invite));
  ca = trim(std::move(ca));
  if (!url.empty()) {
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
      err = "server URL must start with http:// or https://";
      return false;
    }
    if (url.size() > 512 || has_whitespace_or_control(url)) {
      err = "server URL is invalid";
      return false;
    }
  }
  if (invite.size() > 128 || has_whitespace_or_control(invite)) {
    err = "server invite is invalid";
    return false;
  }
  if (ca.size() > 512 || has_whitespace_or_control(ca)) {
    err = "server CA path is invalid";
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  settings_.server_url = url;
  settings_.server_invite = invite;
  settings_.server_ca = ca;
  settings_.server_enabled = enabled;
  persist_locked();
  return true;
}

bool auth_state::set_avatar(std::string avatar, std::string& err) {
  auto trim = [](std::string value) {
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t' ||
            value.back() == '\r' || value.back() == '\n')) {
      value.pop_back();
    }
    std::size_t start = 0;
    while (start < value.size() &&
           (value[start] == ' ' || value[start] == '\t')) {
      ++start;
    }
    return value.substr(start);
  };
  avatar = trim(std::move(avatar));
  if (avatar.size() > 32768) {
    err = "avatar is too large";
    return false;
  }
  if (has_whitespace_or_control(avatar)) {
    err = "avatar contains whitespace or control characters";
    return false;
  }
  if (!avatar.empty() && avatar.rfind("preset:", 0) != 0 &&
      (avatar.rfind("data:image/", 0) != 0 ||
       avatar.find(";base64,") == std::string::npos)) {
    err = "avatar must be a preset or an image data URL";
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  settings_.avatar = std::move(avatar);
  persist_locked();
  return true;
}

void auth_state::update_access(bool allow_remote_access, bool allow_web_client,
                               bool discovery_enabled,
                               const std::string& device_name) {
  std::lock_guard<std::mutex> lock(mutex_);
  settings_.allow_remote_access = allow_remote_access;
  settings_.allow_web_client = allow_web_client;
  settings_.discovery_enabled =
      allow_remote_access ? discovery_enabled : false;
  if (!device_name.empty()) settings_.device_name = device_name;
  persist_locked();
}

bool auth_state::verify_code_locked(const std::string& supplied, bool consume) {
  if (supplied.empty()) return false;
  if (constant_time_equals(supplied, settings_.long_term_code)) return true;
  if (!constant_time_equals(supplied, settings_.temp_code) ||
      settings_.temp_remaining <= 0) {
    return false;
  }
  if (consume) {
    --settings_.temp_remaining;
    if (settings_.temp_remaining <= 0) rotate_temp_locked();
    persist_locked();
  }
  return true;
}

void auth_state::rotate_temp_locked() {
  settings_.temp_uses = settings_.temp_mode == "single" ? 1 : settings_.temp_uses;
  if (settings_.temp_uses < 1) settings_.temp_uses = 1;
  settings_.temp_remaining = settings_.temp_uses;
  settings_.temp_code = generate_numeric(6);
}

void auth_state::invalidate_remote_sessions_locked() {
  remote_sessions_.clear();
}

void auth_state::persist_locked() { save_settings(settings_); }

}  // namespace molink
