// Pure state model for the host-only shell.
//
// Everything here is platform neutral on purpose: the shell's main.cpp owns
// the window and the WinHTTP calls, while the "what does /api/device mean"
// and "how do we phrase it" rules live in this file so they can be compiled
// and tested on the build machine.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace molink {
namespace hostui {

/// GET /api/device.
struct device_info {
  bool valid = false;
  std::string device_id;
  std::string device_name;
  std::string version;
  std::string platform;
  std::string host = "127.0.0.1";
  int port = 0;
  int pid = 0;
  bool tls = false;
  /// Per-launch bearer token for /api/* (not the verification code).
  std::string token;
  /// Per-launch client token for the loopback shell gate.
  std::string client_token;
  std::string code;
  std::string code_mode = "single";
  int code_remaining = 0;
  int code_uses = 0;
  bool long_term_set = false;
  bool single_enabled = true;
  bool multi_enabled = true;
  std::vector<std::string> addresses;
  bool screen_capture = false;
  bool input_injection = false;
  bool clipboard = false;
};

/// GET /api/state.
struct state_info {
  bool valid = false;
  std::string device_id;
  std::string version;
  std::string host = "127.0.0.1";
  int port = 0;
  int bound_port = 0;
  int tcp_port = 0;
  int udp_port = 0;
  bool restart_required = false;
  bool allow_remote_access = false;
  bool host_only = false;
  bool server_enabled = false;
  bool server_online = false;
  std::string server_domain;
  std::string server_display_id;
  std::string last_error;
};

/// GET /api/settings. The long-term code itself is deliberately not kept:
/// the UI only needs to know whether one is set, and a secret should not sit
/// in the shell's snapshot longer than the response body does.
struct settings_info {
  bool valid = false;
  bool allow_remote_access = false;
  bool discovery_enabled = false;
  bool host_only = false;
  std::string access_policy = "full";
  bool long_term_set = false;
};

/// One poll result: the core answers plus the Windows service state.
struct host_snapshot {
  bool core_online = false;
  device_info device;
  state_info state;
  settings_info settings;
  bool service_installed = false;
  bool service_running = false;
  std::string service_state_text;
  /// Filled while offline: why the last connection/spawn attempt failed.
  std::string error;
};

/// Parse a /api/device response. Returns false and fills @p error when the
/// mandatory fields (device_id, port) are missing, which also covers an
/// unrelated HTTP server occupying the port.
bool parse_device(const std::string& body, device_info& out,
                  std::string& error);
bool parse_state(const std::string& body, state_info& out, std::string& error);
bool parse_settings(const std::string& body, settings_info& out,
                    std::string& error);

/// "127.0.0.1:28418（仅本机）" or "192.168.1.8:28418, 10.0.0.5:28418".
/// When remote access is off only loopback is advertised, matching how the
/// core gates non-loopback peers.
std::string join_addresses(const device_info& device, bool allow_remote,
                           int port, std::size_t max_hosts = 2);

/// "123456（多次，剩余 20 次）" / "123456（单次）" / "—".
std::string code_summary(const device_info& device);

/// "在线" / "离线".
std::string online_text(bool core_online);

/// The settings response wins; fall back to /api/state while it is loading.
bool effective_remote_access(const settings_info& settings,
                             const state_info& state);

/// "完整控制" / "仅协助（可输入）" / "仅查看".
std::string access_policy_text(const std::string& policy);

/// "版本 0.12.31" or "版本 —" when the core has not answered yet.
std::string format_version(const std::string& version);

}  // namespace hostui
}  // namespace molink
