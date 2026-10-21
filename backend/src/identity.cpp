#include "identity.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "config.h"

namespace molink {
namespace {

namespace fs = std::filesystem;

std::string env(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : std::string();
}

std::string make_device_id() {
  static const char kAlphabet[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
  static constexpr std::size_t kSize = sizeof(kAlphabet) - 1;
  std::random_device rd;
  std::string id;
  id.reserve(11);
  for (int group = 0; group < 3; ++group) {
    if (group != 0) id.push_back('-');
    for (int i = 0; i < 3; ++i) id.push_back(kAlphabet[rd() % kSize]);
  }
  return id;
}

bool read_file(const fs::path& path, std::string& out) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return false;
  out.assign(std::istreambuf_iterator<char>(stream),
             std::istreambuf_iterator<char>());
  while (!out.empty() &&
         (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
    out.pop_back();
  }
  return !out.empty();
}

void write_file(const fs::path& path, const std::string& value) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) return;
  stream << value << "\n";
  stream.close();
#if !defined(_WIN32)
  ::chmod(path.c_str(), 0600);
#endif
}

}  // namespace

int load_lock_timeout_setting(int fallback) {
  const std::string dir = config_directory();
  if (dir.empty()) return fallback;
  std::string value;
  if (!read_file(fs::path(dir) / "lock_timeout", value)) return fallback;
  try {
    const int seconds = std::stoi(value);
    if (seconds < 0 || seconds > 86400) return fallback;
    return seconds;
  } catch (...) {
    return fallback;
  }
}

void save_lock_timeout_setting(int seconds) {
  const std::string dir = config_directory();
  if (dir.empty()) return;
  write_file(fs::path(dir) / "lock_timeout", std::to_string(seconds));
}

std::string config_directory() {
  const std::string override_dir = env("MOLINK_CONFIG_DIR");
  if (!override_dir.empty()) return override_dir;

#if defined(_WIN32)
  PWSTR appdata = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr,
                                     &appdata))) {
    std::string path;
    const int needed =
        WideCharToMultiByte(CP_UTF8, 0, appdata, -1, nullptr, 0, nullptr, nullptr);
    if (needed > 1) {
      path.resize(static_cast<std::size_t>(needed - 1));
      WideCharToMultiByte(CP_UTF8, 0, appdata, -1, path.data(), needed, nullptr,
                          nullptr);
    }
    CoTaskMemFree(appdata);
    if (!path.empty()) return path + "\\moLink";
  }
  const std::string appdata_env = env("APPDATA");
  return appdata_env.empty() ? std::string() : appdata_env + "\\moLink";
#elif defined(__APPLE__)
  const std::string home = env("HOME");
  return home.empty() ? std::string()
                      : home + "/Library/Application Support/moLink";
#else
  const std::string xdg = env("XDG_CONFIG_HOME");
  if (!xdg.empty()) return xdg + "/molink";
  const std::string home = env("HOME");
  return home.empty() ? std::string() : home + "/.config/molink";
#endif
}

device_identity load_identity(const core_config& config, std::string& err) {
  device_identity identity;
  identity.token = config.token;

  const std::string dir = config_directory();
  if (dir.empty()) {
    if (identity.token.empty()) identity.token = generate_token();
    identity.device_id = make_device_id();
    err = "cannot determine a configuration directory; identity is ephemeral";
    return identity;
  }

  const fs::path base(dir);
  std::error_code ec;
  fs::create_directories(base, ec);

  if (!read_file(base / "device_id", identity.device_id)) {
    identity.device_id = make_device_id();
    write_file(base / "device_id", identity.device_id);
  }

  if (identity.token.empty()) {
    if (!read_file(base / "token", identity.token)) {
      identity.token = generate_token();
      write_file(base / "token", identity.token);
    }
  }
  return identity;
}

}  // namespace molink
