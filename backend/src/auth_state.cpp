#include "auth_state.h"
#include <chrono>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#ifndef _WIN32
#include <sys/stat.h>
#endif
#include <fstream>
#include <iterator>
#include <random>
#include <string>

#include "config.h"
#include "identity.h"
#include "totp.h"
#include "video_mode.h"

namespace molink {
namespace {

constexpr int kRemoteSessionLimit = 64;
/// How long a session token stays usable. It survives a core restart (an
/// update or a service restart) and a host idle lock, because a viewer that
/// authenticated once cannot present a single-use code again.
constexpr uint64_t kSessionLifetimeMs = 7ull * 24ull * 60ull * 60ull * 1000ull;

uint64_t now_epoch_ms() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

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

/// Calendar day in the local time zone, used for the daily code rotation.
long long epoch_day_now() {
  return static_cast<long long>(std::time(nullptr) / 86400);
}

/// Raw 32-byte key that seals the TOTP secret at rest. It lives beside the
/// other per-installation secrets with owner-only permissions, so copying the
/// `2fa` file alone does not reveal the shared secret.
std::string load_or_create_totp_key() {
  const std::string dir = config_directory();
  if (dir.empty()) return std::string();
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::filesystem::path path = std::filesystem::path(dir) / "2fa.key";
  {
    std::ifstream in(path, std::ios::binary);
    if (in) {
      std::string key((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
      if (key.size() == 32) return key;
    }
  }
  std::string key = totp::random_bytes(32);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return key;
  out.write(key.data(), static_cast<std::streamsize>(key.size()));
  out.close();
#ifndef _WIN32
  ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
  return key;
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
  const long long today = epoch_day_now();
  // The temporary code rotates when the day changes, and the single-use code
  // also rotates on every start: a host that showed yesterday's code must not
  // keep accepting it.
  if (settings_.temp_rotated_day != today ||
      settings_.temp_mode == "single" || settings_.temp_code.empty() ||
      settings_.temp_remaining <= 0 ||
      !valid_temp_code(settings_.temp_code, err)) {
    settings_.temp_remaining = settings_.temp_uses;
    settings_.temp_code = generate_numeric(6);
    settings_.temp_rotated_day = today;
  }
  save_settings(settings_);
  load_sessions_locked();
  load_totp_locked();
}

bool auth_state::authenticate(const std::string& supplied, bool consume) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (valid_session_locked(supplied)) return true;
  return verify_code_locked(supplied, consume);
}

login_result auth_state::login(const std::string& supplied,
                               const std::string& totp, bool consume) {
  std::lock_guard<std::mutex> lock(mutex_);
  login_result result;
  if (valid_session_locked(supplied)) {
    result.ok = true;
    result.session_token = true;
    return result;
  }
  if (!totp_enabled_) {
    result.ok = verify_code_locked(supplied, consume);
    return result;
  }
  // Check the first factor without spending it: a wrong six-digit code must
  // not consume a single-use verification code.
  if (!verify_code_locked(supplied, /*consume=*/false)) return result;
  if (totp.empty()) {
    result.totp_required = true;
    return result;
  }
  if (!verify_totp_code_locked(totp_secret_, totp)) {
    result.totp_invalid = true;
    return result;
  }
  // Both factors are right; now the temporary code may be spent.
  if (consume) verify_code_locked(supplied, /*consume=*/true);
  result.ok = true;
  return result;
}

bool auth_state::totp_required() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return totp_enabled_;
}

totp_view auth_state::totp_status() {
  std::lock_guard<std::mutex> lock(mutex_);
  totp_view view;
  view.enabled = totp_enabled_;
  view.account = settings_.device_name;
  if (totp_enabled_ && !totp_secret_.empty()) {
    view.uri = totp_uri_locked(totp_secret_);
  } else {
    ensure_pending_totp_locked();
    if (!totp_pending_.empty()) {
      view.uri = totp_uri_locked(totp_pending_);
    }
  }
  return view;
}

bool auth_state::enable_totp(const std::string& code, std::string& err) {
  std::lock_guard<std::mutex> lock(mutex_);
  err.clear();
  if (totp_enabled_) {
    err = "two-factor authentication is already enabled";
    return false;
  }
  if (totp_key_.empty()) {
    err = "no secure configuration directory is available";
    return false;
  }
  ensure_pending_totp_locked();
  if (totp_pending_.empty()) {
    err = "no enrollment is in progress";
    return false;
  }
  if (!verify_totp_code_locked(totp_pending_, code)) {
    err = "the six-digit code is not valid";
    return false;
  }
  totp_secret_ = totp_pending_;
  totp_pending_.clear();
  totp_enabled_ = true;
  // Session tokens issued before two-factor was turned on must not bypass it.
  invalidate_remote_sessions_locked();
  save_totp_locked();
  return true;
}

bool auth_state::disable_totp(const std::string& credential, std::string& err) {
  std::lock_guard<std::mutex> lock(mutex_);
  err.clear();
  if (!totp_enabled_) {
    err = "two-factor authentication is not enabled";
    return false;
  }
  if (!verify_code_locked(credential, /*consume=*/true)) {
    err = "the verification code is not valid";
    return false;
  }
  totp_enabled_ = false;
  totp_secret_.clear();
  totp_pending_.clear();
  save_totp_locked();
  return true;
}

bool auth_state::set_access_policy(const std::string& policy) {
  if (policy != "full" && policy != "view_only" && policy != "assist") {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  settings_.access_policy = policy;
  persist_locked();
  return true;
}

void auth_state::ensure_pending_totp_locked() {
  if (totp_enabled_ || !totp_pending_.empty()) return;
  totp_pending_ = totp::generate_secret();
  save_totp_locked();
}

std::string auth_state::totp_uri_locked(const std::string& secret) const {
  return totp::otpauth_uri(totp::base32_encode(secret),
                           settings_.device_name.empty()
                               ? std::string("moLink")
                               : settings_.device_name,
                           "moLink");
}

bool auth_state::verify_totp_code_locked(const std::string& secret,
                                         const std::string& code) const {
  if (secret.empty()) return false;
  return totp::verify(secret, code,
                      static_cast<uint64_t>(std::time(nullptr)));
}

void auth_state::load_totp_locked() {
  totp_key_ = load_or_create_totp_key();
  totp_enabled_ = false;
  totp_secret_.clear();
  totp_pending_.clear();
  const std::string dir = config_directory();
  if (dir.empty() || totp_key_.empty()) return;
  std::ifstream in(dir + "/2fa");
  if (!in) return;
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() &&
           (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
      line.pop_back();
    }
    const std::size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq);
    const std::string value = line.substr(eq + 1);
    if (key == "enabled") {
      totp_enabled_ = value == "1" || value == "true";
    } else if (key == "secret") {
      std::string raw;
      if (totp::open(value, totp_key_, raw)) totp_secret_ = std::move(raw);
    } else if (key == "pending") {
      std::string raw;
      if (totp::open(value, totp_key_, raw)) totp_pending_ = std::move(raw);
    }
  }
  // A lost or replaced key file makes the sealed secret unreadable. Disabling
  // is safer than keeping an enabled flag that can never be satisfied.
  if (totp_enabled_ && totp_secret_.size() != totp::kSecretBytes) {
    totp_enabled_ = false;
    totp_secret_.clear();
  }
}

void auth_state::save_totp_locked() const {
  if (totp_key_.empty()) return;
  const std::string dir = config_directory();
  if (dir.empty()) return;
  const std::filesystem::path path = std::filesystem::path(dir) / "2fa";
  const std::filesystem::path temp = std::filesystem::path(dir) / "2fa.tmp";
  std::ofstream out(temp, std::ios::binary | std::ios::trunc);
  if (!out) return;
  out << "enabled=" << (totp_enabled_ ? 1 : 0) << '\n';
  if (!totp_secret_.empty()) {
    out << "secret=" << totp::seal(totp_secret_, totp_key_) << '\n';
  }
  if (!totp_pending_.empty()) {
    out << "pending=" << totp::seal(totp_pending_, totp_key_) << '\n';
  }
  out.close();
  std::error_code ec;
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    std::filesystem::remove(path, ec);
    std::filesystem::rename(temp, path, ec);
  }
#ifndef _WIN32
  ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
}

bool auth_state::valid_session(const std::string& token) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return valid_session_locked(token);
}

bool auth_state::valid_session_locked(const std::string& token) const {
  if (token.empty()) return false;
  unsigned diff = constant_time_equals(token, local_session_) ? 0u : 1u;
  for (const auto& session : remote_sessions_) {
    if (constant_time_equals(token, session.first)) diff = 0;
  }
  return diff == 0;
}

bool auth_state::valid_recent_session(const std::string& token,
                                      uint64_t max_age_ms) const {
  if (token.empty()) return false;
  const uint64_t now = now_epoch_ms();
  std::lock_guard<std::mutex> lock(mutex_);
  unsigned diff = 1u;
  for (const auto& session : remote_sessions_) {
    if (constant_time_equals(token, session.first) && now >= session.second &&
        now - session.second <= max_age_ms) {
      diff = 0;
    }
  }
  return diff == 0;
}

void auth_state::touch_session(const std::string& token) {
  if (token.empty()) return;
  const uint64_t now = now_epoch_ms();
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& session : remote_sessions_) {
    if (!constant_time_equals(token, session.first)) continue;
    // Rewrite at most once a minute: every reconnect refreshes the lifetime,
    // but an idle session does not rewrite the file on every frame.
    if (now > session.second && now - session.second < 60000) return;
    session.second = now;
    save_sessions_locked();
    return;
  }
}

void auth_state::load_sessions_locked() {
  const std::string dir = config_directory();
  if (dir.empty()) return;
  std::ifstream in(dir + "/sessions");
  if (!in) return;
  const uint64_t now = now_epoch_ms();
  std::string token;
  uint64_t issued = 0;
  while (in >> token >> issued) {
    if (token.empty()) continue;
    const uint64_t age = now >= issued ? now - issued : issued - now;
    if (age > kSessionLifetimeMs) continue;
    remote_sessions_.emplace_back(token, issued);
    if (remote_sessions_.size() >= kRemoteSessionLimit) break;
  }
}

void auth_state::save_sessions_locked() const {
  const std::string dir = config_directory();
  if (dir.empty()) return;
  const std::filesystem::path path =
      std::filesystem::path(dir) / "sessions";
  const std::filesystem::path temp =
      std::filesystem::path(dir) / "sessions.tmp";
  std::ofstream out(temp, std::ios::binary | std::ios::trunc);
  if (!out) return;
  for (const auto& session : remote_sessions_) {
    out << session.first << ' ' << session.second << '\n';
  }
  out.close();
  std::error_code ec;
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    std::filesystem::remove(temp, ec);
    return;
  }
#ifndef _WIN32
  // Session tokens are credentials: keep them owner-only like the settings.
  ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
}

std::string auth_state::create_session() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string token = generate_token();
  remote_sessions_.emplace_back(token, now_epoch_ms());
  while (remote_sessions_.size() > kRemoteSessionLimit) {
    remote_sessions_.pop_front();
  }
  save_sessions_locked();
  return token;
}

code_view auth_state::view() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return view_locked();
}

code_view auth_state::view_locked() const {
  code_view out;
  out.mode = settings_.temp_mode;
  out.value = settings_.temp_code;
  out.remaining = settings_.temp_remaining;
  out.uses = settings_.temp_uses;
  out.long_term_set = !settings_.long_term_code.empty();
  out.single_enabled = settings_.single_code_enabled;
  out.multi_enabled = settings_.multi_code_enabled;
  return out;
}

bool auth_state::temp_code_enabled_locked() const {
  return settings_.temp_mode == "multi" ? settings_.multi_code_enabled
                                        : settings_.single_code_enabled;
}

void auth_state::maybe_rotate_daily_locked() {
  const long long today = epoch_day_now();
  if (settings_.temp_rotated_day == today) return;
  settings_.temp_rotated_day = today;
  settings_.temp_remaining = settings_.temp_uses;
  settings_.temp_code = generate_numeric(6);
  persist_locked();
}

void auth_state::refresh_daily() {
  std::lock_guard<std::mutex> lock(mutex_);
  maybe_rotate_daily_locked();
}

code_view auth_state::set_code_policy(bool single_enabled,
                                      bool multi_enabled, std::string& err) {
  std::lock_guard<std::mutex> lock(mutex_);
  err.clear();
  if (!single_enabled) {
    std::string long_term_error;
    if (!valid_long_term(settings_.long_term_code, long_term_error)) {
      err = "set a long-term code before disabling the single-use code";
      return view_locked();
    }
  }
  settings_.single_code_enabled = single_enabled;
  settings_.multi_code_enabled = multi_enabled;
  persist_locked();
  return view_locked();
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
  if ((mode == "single" && !settings_.single_code_enabled) ||
      (mode == "multi" && !settings_.multi_code_enabled)) {
    err = "that verification mode is disabled";
    return view_locked();
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

  return view_locked();
}

bool auth_state::set_video_mode(const std::string& mode) {
  // "auto" selects the adaptive path; "mixed" is the same request from an
  // older client. Both are stored as auto so the welcome and the toolbar
  // report the mode the host actually runs.
  if (mode != "auto" && mode != "stream" && mode != "mixed" &&
      mode != "tiles") {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  settings_.video_mode = normalize_video_mode(mode);
  persist_locked();
  return true;
}

bool auth_state::set_stream_codec(const std::string& codec) {
  // "tiles" is the explicit never-stream choice offered by the toolbar.
  if (codec != "auto" && codec != "tiles" && codec != "av1" &&
      codec != "h264" && codec != "vp9" && codec != "vp8") {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  settings_.stream_codec = codec;
  persist_locked();
  return true;
}

void auth_state::set_stream_overrides(int qp, int bitrate_kbps, int fps,
                                      const std::string& rate_mode,
                                      const std::string& format) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (qp >= 1 && qp <= 63) settings_.stream_qp = qp;
  else if (qp == 0) settings_.stream_qp = 0;
  if (bitrate_kbps >= 0) settings_.stream_bitrate_kbps = bitrate_kbps;
  if (fps >= 0 && fps <= 60) settings_.stream_fps = fps;
  if (rate_mode == "auto" || rate_mode == "cq" || rate_mode == "cbr") {
    settings_.stream_rate_mode = rate_mode;
  }
  if (format.empty() || format == "gray8" || format == "rgb565" ||
      format == "rgb24" || format == "bgra") {
    settings_.stream_format = format;
  }
  persist_locked();
}

bool auth_state::set_audio_quality(const std::string& quality) {
  if (quality != "high" && quality != "balanced" && quality != "low") {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  settings_.audio_quality = quality;
  persist_locked();
  return true;
}

void auth_state::set_allow_access_requests(bool allow) {
  std::lock_guard<std::mutex> lock(mutex_);
  settings_.allow_access_requests = allow;
  persist_locked();
}

void auth_state::set_allow_sync_between_sessions(bool allow) {
  std::lock_guard<std::mutex> lock(mutex_);
  settings_.allow_sync_between_sessions = allow;
  persist_locked();
}

void auth_state::set_file_sandbox(bool enabled, std::string roots) {
  std::lock_guard<std::mutex> lock(mutex_);
  settings_.file_sandbox = enabled;
  settings_.file_sandbox_roots = std::move(roots);
  persist_locked();
}

void auth_state::set_ip_allowlist(std::string value) {
  std::lock_guard<std::mutex> lock(mutex_);
  settings_.ip_allowlist = std::move(value);
  persist_locked();
}

void auth_state::set_transport_security(bool encrypt_remote,
                                        bool encrypt_lan) {
  std::lock_guard<std::mutex> lock(mutex_);
  settings_.encrypt_remote = encrypt_remote;
  settings_.encrypt_lan = encrypt_lan;
  persist_locked();
}

bool auth_state::set_stun_server(const std::string& value) {
  std::string trimmed = value;
  while (!trimmed.empty() &&
         (trimmed.back() == ' ' || trimmed.back() == '\t' ||
          trimmed.back() == '\r' || trimmed.back() == '\n')) {
    trimmed.pop_back();
  }
  std::size_t start = 0;
  while (start < trimmed.size() &&
         (trimmed[start] == ' ' || trimmed[start] == '\t')) {
    ++start;
  }
  trimmed = trimmed.substr(start);
  if (trimmed.size() > 255 || has_whitespace_or_control(trimmed)) {
    return false;
  }
  if (trimmed != "none" && trimmed != "off" && trimmed != "0") {
    const std::size_t colon = trimmed.rfind(':');
    if (colon != std::string::npos && trimmed.find(':') == colon) {
      if (colon == 0) return false;
      const std::string port = trimmed.substr(colon + 1);
      if (!port.empty()) {
        try {
          const int parsed = std::stoi(port);
          if (parsed < 1 || parsed > 65535) return false;
        } catch (...) {
          return false;
        }
      }
    }
  }
  std::lock_guard<std::mutex> lock(mutex_);
  settings_.stun_server = trimmed;
  persist_locked();
  return true;
}

void auth_state::set_service_desired_running(bool enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  settings_.service_desired_running = enabled;
  persist_locked();
}

void auth_state::set_diag_logs_enabled(bool enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  settings_.diag_logs_enabled = enabled;
  persist_locked();
}

void auth_state::set_av1_encoding(bool enabled, std::string device,
                                  std::string speed, int qp,
                                  int max_area_percent, int threads,
                                  bool full_frame) {
  std::lock_guard<std::mutex> lock(mutex_);
  settings_.av1_enabled = enabled;
  settings_.av1_full_frame = full_frame;
  settings_.av1_device = std::move(device);
  settings_.av1_speed = std::move(speed);
  settings_.av1_qp = qp;
  settings_.av1_max_area_percent = max_area_percent;
  settings_.av1_threads = threads;
  persist_locked();
}

void auth_state::set_keyframe_interval_seconds(int seconds) {
  std::lock_guard<std::mutex> lock(mutex_);
  settings_.keyframe_interval_seconds = seconds;
  persist_locked();
}

void auth_state::set_stream_enter_percent(int percent) {
  std::lock_guard<std::mutex> lock(mutex_);
  settings_.stream_enter_percent = percent < 1 || percent > 100 ? 15 : percent;
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
  maybe_rotate_daily_locked();
  if (constant_time_equals(supplied, settings_.long_term_code)) return true;
  // A disabled mode's code is not accepted even if the digits still match.
  if (!temp_code_enabled_locked()) return false;
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
  settings_.temp_rotated_day = epoch_day_now();
}

void auth_state::invalidate_remote_sessions_locked() {
  remote_sessions_.clear();
  save_sessions_locked();
}

void auth_state::persist_locked() { save_settings(settings_); }

}  // namespace molink
