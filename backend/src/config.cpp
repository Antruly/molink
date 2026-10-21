#include "config.h"

#include <cctype>
#include <cstdlib>
#include <random>

namespace molink {
namespace {

bool parse_int(const std::string& s, int lo, int hi, int& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  const long v = std::strtol(s.c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || v < lo || v > hi) return false;
  out = static_cast<int>(v);
  return true;
}

std::string env_or_empty(const char* name) {
  const char* v = std::getenv(name);
  return v == nullptr ? std::string() : std::string(v);
}

}  // namespace

bool parse_args(int argc, char** argv, core_config& cfg, std::string& err) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&](std::string& dst) -> bool {
      if (i + 1 >= argc) {
        err = "missing value for " + arg;
        return false;
      }
      dst = argv[++i];
      return true;
    };

    if (arg == "--help" || arg == "-h") {
      cfg.show_help = true;
      return true;
    }
    if (arg == "--version") {
      cfg.show_version = true;
      return true;
    }
    if (arg == "--host") {
      if (!value(cfg.host)) return false;
      continue;
    }
    if (arg == "--port") {
      std::string v;
      if (!value(v)) return false;
      // 0 asks the OS for an ephemeral port; the shell relies on that.
      if (!parse_int(v, 0, 65535, cfg.port)) {
        err = "invalid --port: " + v;
        return false;
      }
      continue;
    }
    if (arg == "--token") {
      if (!value(cfg.token)) return false;
      continue;
    }
    if (arg == "--print-ready") {
      cfg.print_ready_json = true;
      continue;
    }
    if (arg == "--allow-remote") {
      cfg.allow_remote = true;
      continue;
    }
    if (arg == "--parent-pid") {
      std::string v;
      if (!value(v)) return false;
      char* end = nullptr;
      const long pid = std::strtol(v.c_str(), &end, 10);
      if (end == nullptr || *end != '\0' || pid <= 0) {
        err = "invalid --parent-pid: " + v;
        return false;
      }
      cfg.parent_pid = pid;
      continue;
    }
    if (arg == "--capture-fps") {
      std::string v;
      if (!value(v)) return false;
      if (!parse_int(v, 1, 60, cfg.capture_fps)) {
        err = "invalid --capture-fps: " + v;
        return false;
      }
      continue;
    }
    if (arg == "--no-capture") {
      cfg.no_capture = true;
      continue;
    }
    if (arg == "--no-input") {
      cfg.no_input = true;
      continue;
    }
    if (arg == "--no-compress") {
      cfg.no_compress = true;
      continue;
    }
    if (arg == "--no-clipboard") {
      cfg.no_clipboard = true;
      continue;
    }
    if (arg == "--web-root") {
      if (!value(cfg.web_root)) return false;
      continue;
    }
    if (arg == "--lock-timeout") {
      std::string v;
      if (!value(v)) return false;
      if (!parse_int(v, 0, 86400, cfg.lock_timeout_seconds)) {
        err = "invalid --lock-timeout: " + v;
        return false;
      }
      cfg.lock_timeout_explicit = true;
      continue;
    }
    err = "unknown argument: " + arg;
    return false;
  }

  if (cfg.token.empty()) cfg.token = env_or_empty("MOLINK_TOKEN");

  // Binding to anything but loopback exposes screen contents and input to the
  // local network. It is a deliberate opt-in, not a default.
  if (!cfg.allow_remote && cfg.host != "127.0.0.1" && cfg.host != "::1" &&
      cfg.host != "localhost") {
    err = "--host " + cfg.host +
          " is not loopback; pass --allow-remote to confirm";
    return false;
  }
  return true;
}

std::string generate_token() {
  static const char kHex[] = "0123456789abcdef";
  std::random_device rd;
  std::string out;
  out.reserve(32);
  for (int i = 0; i < 16; ++i) {
    const unsigned byte = static_cast<unsigned>(rd() & 0xffu);
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 0x0fu]);
  }
  return out;
}

bool constant_time_equals(const std::string& a, const std::string& b) {
  const std::size_t n = a.size() > b.size() ? a.size() : b.size();
  unsigned diff = static_cast<unsigned>(a.size() ^ b.size());
  for (std::size_t i = 0; i < n; ++i) {
    const unsigned ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0u;
    const unsigned cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0u;
    diff |= ca ^ cb;
  }
  return diff == 0;
}

}  // namespace molink
