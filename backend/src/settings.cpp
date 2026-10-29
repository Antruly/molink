#include "settings.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "identity.h"
#include "version.h"

namespace molink {
namespace {

namespace fs = std::filesystem;

std::string trim(std::string value) {
  while (!value.empty() &&
         (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' ||
          value.back() == '\t')) {
    value.pop_back();
  }
  std::size_t start = 0;
  while (start < value.size() &&
         (value[start] == ' ' || value[start] == '\t')) {
    ++start;
  }
  return value.substr(start);
}

bool parse_bool(const std::string& value, bool fallback) {
  if (value == "1" || value == "true" || value == "yes" || value == "on") {
    return true;
  }
  if (value == "0" || value == "false" || value == "no" || value == "off") {
    return false;
  }
  return fallback;
}

int parse_int(const std::string& value, int fallback) {
  try {
    return std::stoi(value);
  } catch (...) {
    return fallback;
  }
}

/// Cloudflare Origin CA root (https://developers.cloudflare.com/ssl/origin-configuration/origin-ca/).
/// The built-in directory's certificate is issued by this CA; public trust
/// stores do not include it, so the client carries a copy.
constexpr char kDirectoryCaPem[] = R"PEM(-----BEGIN CERTIFICATE-----
MIIEADCCAuigAwIBAgIID+rOSdTGfGcwDQYJKoZIhvcNAQELBQAwgYsxCzAJBgNV
BAYTAlVTMRkwFwYDVQQKExBDbG91ZEZsYXJlLCBJbmMuMTQwMgYDVQQLEytDbG91
ZEZsYXJlIE9yaWdpbiBTU0wgQ2VydGlmaWNhdGUgQXV0aG9yaXR5MRYwFAYDVQQH
Ew1TYW4gRnJhbmNpc2NvMRMwEQYDVQQIEwpDYWxpZm9ybmlhMB4XDTE5MDgyMzIx
MDgwMFoXDTI5MDgxNTE3MDAwMFowgYsxCzAJBgNVBAYTAlVTMRkwFwYDVQQKExBD
bG91ZEZsYXJlLCBJbmMuMTQwMgYDVQQLEytDbG91ZEZsYXJlIE9yaWdpbiBTU0wg
Q2VydGlmaWNhdGUgQXV0aG9yaXR5MRYwFAYDVQQHEw1TYW4gRnJhbmNpc2NvMRMw
EQYDVQQIEwpDYWxpZm9ybmlhMIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKC
AQEAwEiVZ/UoQpHmFsHvk5isBxRehukP8DG9JhFev3WZtG76WoTthvLJFRKFCHXm
V6Z5/66Z4S09mgsUuFwvJzMnE6Ej6yIsYNCb9r9QORa8BdhrkNn6kdTly3mdnykb
OomnwbUfLlExVgNdlP0XoRoeMwbQ4598foiHblO2B/LKuNfJzAMfS7oZe34b+vLB
yrP/1bgCSLdc1AxQc1AC0EsQQhgcyTJNgnG4va1c7ogPlwKyhbDyZ4e59N5lbYPJ
SmXI/cAe3jXj1FBLJZkwnoDKe0v13xeF+nF32smSH0qB7aJX2tBMW4TWtFPmzs5I
lwrFSySWAdwYdgxw180yKU0dvwIDAQABo2YwZDAOBgNVHQ8BAf8EBAMCAQYwEgYD
VR0TAQH/BAgwBgEB/wIBAjAdBgNVHQ4EFgQUJOhTV118NECHqeuU27rhFnj8KaQw
HwYDVR0jBBgwFoAUJOhTV118NECHqeuU27rhFnj8KaQwDQYJKoZIhvcNAQELBQAD
ggEBAHwOf9Ur1l0Ar5vFE6PNrZWrDfQIMyEfdgSKofCdTckbqXNTiXdgbHs+TWoQ
wAB0pfJDAHJDXOTCWRyTeXOseeOi5Btj5CnEuw3P0oXqdqevM1/+uWp0CM35zgZ8
VD4aITxity0djzE6Qnx3Syzz+ZkoBgTnNum7d9A66/V636x4vTeqbZFBr9erJzgz
hhurjcoacvRNhnjtDRM0dPeiCJ50CP3wEYuvUzDHUaowOsnLCjQIkWbR7Ni6KEIk
MOz2U0OBSif3FTkhCgZWQKOOLo1P42jHC3ssUZAtVNXrCk3fw9/E15k8NPkBazZ6
0iykLhH1trywrKRMVw67F44IE8Y=
-----END CERTIFICATE-----
)PEM";

/// Write the bundled CA into the config directory (where the process can read
/// it) and return its path. Returns empty when the directory is unwritable.
std::string bundled_directory_ca() {
  const std::string dir = config_directory();
  if (dir.empty()) return std::string();
  std::error_code ec;
  fs::create_directories(dir, ec);
  const fs::path path = fs::path(dir) / "directory-ca.pem";
  const std::string pem(kDirectoryCaPem);
  std::ifstream existing(path, std::ios::binary);
  if (existing) {
    const std::string current((std::istreambuf_iterator<char>(existing)),
                              std::istreambuf_iterator<char>());
    if (current == pem) return path.string();
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return std::string();
  out << pem;
  out.close();
  if (!out) return std::string();
  return path.string();
}

}  // namespace

std::string default_server_url() { return MOLINK_DEFAULT_SERVER_URL; }

std::string effective_server_url(const app_settings& settings) {
  if (!settings.server_enabled) return std::string();
  const std::string custom = trim(settings.server_url);
  return custom.empty() ? default_server_url() : custom;
}

std::string effective_server_ca(const app_settings& settings) {
  const std::string custom = trim(settings.server_ca);
  if (!custom.empty()) return custom;
  if (effective_server_url(settings) != default_server_url()) {
    return std::string();
  }
  return bundled_directory_ca();
}

std::string default_device_name() {
#if defined(_WIN32)
  const char* name = std::getenv("COMPUTERNAME");
  if (name != nullptr && name[0] != '\0') return name;
#else
  char host[256] = {};
  if (::gethostname(host, sizeof(host) - 1) == 0 && host[0] != '\0') {
    return host;
  }
#endif
  return "moLink";
}

app_settings load_settings() {
  app_settings settings;
  settings.device_name = default_device_name();

  const std::string dir = config_directory();
  if (dir.empty()) return settings;
  std::ifstream stream(fs::path(dir) / "settings");
  if (!stream) return settings;

  std::string line;
  while (std::getline(stream, line)) {
    line = trim(std::move(line));
    if (line.empty() || line[0] == '#') continue;
    const std::size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = trim(line.substr(0, eq));
    const std::string value = trim(line.substr(eq + 1));
    if (key == "allow_remote_access") {
      settings.allow_remote_access = parse_bool(value, false);
    } else if (key == "allow_web_client") {
      settings.allow_web_client = parse_bool(value, false);
    } else if (key == "discovery_enabled") {
      settings.discovery_enabled = parse_bool(value, false);
    } else if (key == "allow_access_requests") {
      settings.allow_access_requests = parse_bool(value, true);
    } else if (key == "encrypt_remote") {
      settings.encrypt_remote = parse_bool(value, true);
    } else if (key == "encrypt_lan") {
      settings.encrypt_lan = parse_bool(value, false);
    } else if (key == "service_desired_running") {
      settings.service_desired_running = parse_bool(value, true);
    } else if (key == "av1_enabled") {
      settings.av1_enabled = parse_bool(value, true);
    } else if (key == "av1_full_frame") {
      settings.av1_full_frame = parse_bool(value, true);
    } else if (key == "av1_device") {
      settings.av1_device = value.empty() ? "auto" : value;
    } else if (key == "av1_speed") {
      if (value == "auto" || value == "fast" || value == "balanced" ||
          value == "quality") {
        settings.av1_speed = value;
      }
    } else if (key == "av1_qp") {
      settings.av1_qp = parse_int(value, 0);
    } else if (key == "av1_max_area_percent") {
      settings.av1_max_area_percent = parse_int(value, 0);
    } else if (key == "av1_threads") {
      settings.av1_threads = parse_int(value, 0);
    } else if (key == "keyframe_interval_seconds") {
      settings.keyframe_interval_seconds = parse_int(value, 0);
    } else if (key == "device_name") {
      if (!value.empty()) settings.device_name = value;
    } else if (key == "server_enabled") {
      settings.server_enabled = parse_bool(value, true);
    } else if (key == "avatar") {
      settings.avatar = value;
    } else if (key == "server_url") {
      settings.server_url = value;
    } else if (key == "server_invite") {
      settings.server_invite = value;
    } else if (key == "server_ca") {
      settings.server_ca = value;
    } else if (key == "stun_server") {
      settings.stun_server = value;
    } else if (key == "stream_enter_percent") {
      settings.stream_enter_percent = parse_int(value, 15);
    } else if (key == "video_mode") {
      if (value == "stream" || value == "mixed" || value == "tiles") {
        settings.video_mode = value;
      }
    } else if (key == "stream_codec") {
      if (value == "auto" || value == "av1" || value == "h264" ||
          value == "vp9" || value == "vp8") {
        settings.stream_codec = value;
      }
    } else if (key == "long_term_code") {
      settings.long_term_code = value;
    } else if (key == "temp_mode") {
      if (value == "single" || value == "multi") settings.temp_mode = value;
    } else if (key == "temp_code") {
      settings.temp_code = value;
    } else if (key == "temp_uses") {
      settings.temp_uses = parse_int(value, 1);
    } else if (key == "temp_remaining") {
      settings.temp_remaining = parse_int(value, -1);
    } else if (key == "single_code_enabled") {
      settings.single_code_enabled = parse_bool(value, true);
    } else if (key == "multi_code_enabled") {
      settings.multi_code_enabled = parse_bool(value, true);
    } else if (key == "temp_rotated_day") {
      settings.temp_rotated_day = parse_int(value, 0);
    }
  }
  if (settings.temp_uses < 1) settings.temp_uses = 1;
  if (settings.temp_uses > 10000) settings.temp_uses = 10000;
  if (settings.temp_remaining > settings.temp_uses) {
    settings.temp_remaining = settings.temp_uses;
  }
  if (settings.av1_qp < 0 || settings.av1_qp > 63) settings.av1_qp = 0;
  if (settings.av1_max_area_percent < 0 ||
      settings.av1_max_area_percent > 100) {
    settings.av1_max_area_percent = 0;
  }
  if (settings.av1_threads < 0 || settings.av1_threads > 64) {
    settings.av1_threads = 0;
  }
  if (settings.keyframe_interval_seconds < 0 ||
      settings.keyframe_interval_seconds > 60) {
    settings.keyframe_interval_seconds = 0;
  }
  if (settings.stream_enter_percent < 1 ||
      settings.stream_enter_percent > 100) {
    settings.stream_enter_percent = 15;
  }
  // Discovery cannot be on without remote access.
  if (!settings.allow_remote_access) settings.discovery_enabled = false;
  return settings;
}

void save_settings(const app_settings& settings) {
  const std::string dir = config_directory();
  if (dir.empty()) return;

  std::error_code ec;
  fs::create_directories(dir, ec);
  const fs::path path = fs::path(dir) / "settings";
  const fs::path tmp = fs::path(dir) / "settings.tmp";

  std::ofstream stream(tmp, std::ios::binary | std::ios::trunc);
  if (!stream) return;
  stream << "# moLink settings. Contains secrets; keep this file private.\n";
  stream << "allow_remote_access=" << (settings.allow_remote_access ? 1 : 0)
         << "\n";
  stream << "allow_web_client=" << (settings.allow_web_client ? 1 : 0) << "\n";
  stream << "discovery_enabled=" << (settings.discovery_enabled ? 1 : 0)
         << "\n";
  stream << "allow_access_requests="
         << (settings.allow_access_requests ? 1 : 0) << "\n";
  stream << "encrypt_remote=" << (settings.encrypt_remote ? 1 : 0) << "\n";
  stream << "encrypt_lan=" << (settings.encrypt_lan ? 1 : 0) << "\n";
  stream << "service_desired_running="
         << (settings.service_desired_running ? 1 : 0) << "\n";
  stream << "av1_enabled=" << (settings.av1_enabled ? 1 : 0) << "\n";
  stream << "av1_full_frame=" << (settings.av1_full_frame ? 1 : 0) << "\n";
  stream << "av1_device=" << settings.av1_device << "\n";
  stream << "av1_speed=" << settings.av1_speed << "\n";
  stream << "av1_qp=" << settings.av1_qp << "\n";
  stream << "av1_max_area_percent=" << settings.av1_max_area_percent << "\n";
  stream << "av1_threads=" << settings.av1_threads << "\n";
  stream << "keyframe_interval_seconds="
         << settings.keyframe_interval_seconds << "\n";
  stream << "device_name=" << settings.device_name << "\n";
  stream << "avatar=" << settings.avatar << "\n";
  stream << "server_enabled=" << (settings.server_enabled ? 1 : 0) << "\n";
  stream << "server_url=" << settings.server_url << "\n";
  stream << "server_invite=" << settings.server_invite << "\n";
  stream << "server_ca=" << settings.server_ca << "\n";
  stream << "stun_server=" << settings.stun_server << "\n";
  stream << "video_mode=" << settings.video_mode << "\n";
  stream << "stream_enter_percent=" << settings.stream_enter_percent << "\n";
  stream << "stream_codec=" << settings.stream_codec << "\n";
  stream << "long_term_code=" << settings.long_term_code << "\n";
  stream << "temp_mode=" << settings.temp_mode << "\n";
  stream << "temp_code=" << settings.temp_code << "\n";
  stream << "temp_uses=" << settings.temp_uses << "\n";
  stream << "temp_remaining=" << settings.temp_remaining << "\n";
  stream << "single_code_enabled=" << (settings.single_code_enabled ? 1 : 0)
         << "\n";
  stream << "multi_code_enabled=" << (settings.multi_code_enabled ? 1 : 0)
         << "\n";
  stream << "temp_rotated_day=" << settings.temp_rotated_day << "\n";
  stream.close();

#if !defined(_WIN32)
  ::chmod(tmp.c_str(), 0600);
#endif
  fs::rename(tmp, path, ec);
  if (ec) {
    // Windows fs::rename does not replace an existing file.
    fs::remove(path, ec);
    fs::rename(tmp, path, ec);
  }
#if !defined(_WIN32)
  ::chmod(path.c_str(), 0600);
#endif
}

}  // namespace molink
