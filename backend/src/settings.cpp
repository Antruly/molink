#include "settings.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "identity.h"

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

}  // namespace

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
    } else if (key == "service_desired_running") {
      settings.service_desired_running = parse_bool(value, true);
    } else if (key == "device_name") {
      if (!value.empty()) settings.device_name = value;
    } else if (key == "server_url") {
      settings.server_url = value;
    } else if (key == "server_invite") {
      settings.server_invite = value;
    } else if (key == "long_term_code") {
      settings.long_term_code = value;
    } else if (key == "temp_mode") {
      if (value == "single" || value == "multi") settings.temp_mode = value;
    } else if (key == "temp_code") {
      settings.temp_code = value;
    } else if (key == "temp_uses") {
      settings.temp_uses = parse_int(value, 20);
    } else if (key == "temp_remaining") {
      settings.temp_remaining = parse_int(value, -1);
    }
  }
  if (settings.temp_uses < 1) settings.temp_uses = 20;
  if (settings.temp_uses > 10000) settings.temp_uses = 10000;
  if (settings.temp_remaining > settings.temp_uses) {
    settings.temp_remaining = settings.temp_uses;
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
  stream << "service_desired_running="
         << (settings.service_desired_running ? 1 : 0) << "\n";
  stream << "device_name=" << settings.device_name << "\n";
  stream << "server_url=" << settings.server_url << "\n";
  stream << "server_invite=" << settings.server_invite << "\n";
  stream << "long_term_code=" << settings.long_term_code << "\n";
  stream << "temp_mode=" << settings.temp_mode << "\n";
  stream << "temp_code=" << settings.temp_code << "\n";
  stream << "temp_uses=" << settings.temp_uses << "\n";
  stream << "temp_remaining=" << settings.temp_remaining << "\n";
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
