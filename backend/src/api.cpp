#include "api.h"

#include "service_control.h"

#ifdef _WIN32
// libuv includes winsock2.h. Keep windows.h from pulling the old winsock.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_json.h>

#ifndef MOLINK_HAVE_NVENC
#define MOLINK_HAVE_NVENC 0
#endif
#if MOLINK_HAVE_NVENC
#include "capture/nvenc_av1_encoder.h"
#endif

#include "clipboard_stage.h"
#include "clipboard_virtual.h"
#include "clipboard/virtual_mount.h"
#include "config.h"
#include "directory_listing.h"
#include "discovery.h"
#include "identity.h"
#include "ui_store.h"
#include "util.h"
#include "version.h"
#ifdef _WIN32
#include <process.h>
#define MOLINK_GETPID _getpid
#else
#include <unistd.h>
#define MOLINK_GETPID getpid
#endif

namespace molink {
namespace {

/// The clipboard is an asynchronous X11/Win32 conversation, but the HTTP
/// handler runs on the loop thread. Keep the last answered file list here:
/// each poll starts a background refresh and returns the previous snapshot,
/// so a slow clipboard owner never stalls frames or input.
struct clipboard_files_snapshot {
  std::mutex mutex;
  std::vector<clipboard_file> files;
  uint64_t version = 0;
  std::atomic<bool> fetching{false};
};

std::shared_ptr<clipboard_files_snapshot> clipboard_snapshot() {
  static auto snapshot = std::make_shared<clipboard_files_snapshot>();
  return snapshot;
}

/// Paths this core has just written to its own clipboard by applying a staged
/// remote selection. The page's local-file sync reads /api/clipboard-files;
/// marking these entries prevents it from offering the same files straight
/// back to the remote (the (1)(2) echo loop). Entries expire after the same
/// two minutes the page-side echo table uses.
struct clipboard_echo_marks {
  std::mutex mutex;
  std::map<std::string, uint64_t> applied;
};

constexpr uint64_t kClipboardEchoTtlMs = 2ull * 60 * 1000;
constexpr std::size_t kClipboardEchoMax = 64;

std::string echo_path_key(std::string path) {
#if defined(_WIN32)
  std::transform(path.begin(), path.end(), path.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
#endif
  for (char& c : path) {
    if (c == '\\') c = '/';
  }
  return path;
}

std::shared_ptr<clipboard_echo_marks> clipboard_echoes() {
  static auto marks = std::make_shared<clipboard_echo_marks>();
  return marks;
}

namespace fs = std::filesystem;

std::string local_home_directory() {
#if defined(_WIN32)
  const char* profile = std::getenv("USERPROFILE");
  if (profile != nullptr && *profile != '\0') return profile;
  const char* drive = std::getenv("HOMEDRIVE");
  const char* path = std::getenv("HOMEPATH");
  if (drive != nullptr && path != nullptr) return std::string(drive) + path;
  return "C:\\";
#else
  const char* home = std::getenv("HOME");
  return home != nullptr && *home != '\0' ? home : "/";
#endif
}

/// Where files copied from a remote clipboard are materialised. They are cache
/// data (the OS clipboard references them), not configuration, so they live
/// under the platform cache directory: `~/.cache/molink/clipboard/<sha1>/` on
/// Linux, `%LOCALAPPDATA%\moLink\cache\clipboard\<sha1>\` on Windows and
/// `~/Library/Caches/moLink/clipboard/<sha1>/` on macOS.
std::string clipboard_cache_directory() {
#if defined(_WIN32)
  const char* local = std::getenv("LOCALAPPDATA");
  const std::string base =
      local != nullptr && *local != '\0'
          ? std::string(local) + "\\moLink\\cache"
          : local_home_directory() + "\\AppData\\Local\\moLink\\cache";
  return base + "\\clipboard";
#elif defined(__APPLE__)
  return local_home_directory() + "/Library/Caches/moLink/clipboard";
#else
  const char* xdg = std::getenv("XDG_CACHE_HOME");
  const std::string base =
      xdg != nullptr && *xdg != '\0' ? std::string(xdg)
                                     : local_home_directory() + "/.cache";
  return base + "/molink/clipboard";
#endif
}

/// Where the viewer mounts remote clipboard files for on-demand reads:
/// `~/.cache/molink/clip/<generation>/` on Linux. Only the FUSE path uses it;
/// other platforms fall back to the staging directories above.
std::string clipboard_virtual_directory() {
#if defined(_WIN32)
  const char* local = std::getenv("LOCALAPPDATA");
  const std::string base =
      local != nullptr && *local != '\0'
          ? std::string(local) + "\\moLink\\cache"
          : local_home_directory() + "\\AppData\\Local\\moLink\\cache";
  return base + "\\clip";
#elif defined(__APPLE__)
  return local_home_directory() + "/Library/Caches/moLink/clip";
#else
  const char* xdg = std::getenv("XDG_CACHE_HOME");
  const std::string base =
      xdg != nullptr && *xdg != '\0' ? std::string(xdg)
                                     : local_home_directory() + "/.cache";
  return base + "/molink/clip";
#endif
}

void remember_clipboard_echoes(const std::vector<std::string>& paths) {
  const auto marks = clipboard_echoes();
  const uint64_t now = now_ms();
  std::lock_guard<std::mutex> lock(marks->mutex);
  for (const std::string& path : paths) {
    marks->applied[echo_path_key(path)] = now;
  }
  for (auto it = marks->applied.begin(); it != marks->applied.end();) {
    if (now > it->second && now - it->second >= kClipboardEchoTtlMs) {
      it = marks->applied.erase(it);
    } else {
      ++it;
    }
  }
  while (marks->applied.size() > kClipboardEchoMax) {
    auto oldest = marks->applied.begin();
    for (auto it = marks->applied.begin(); it != marks->applied.end(); ++it) {
      if (it->second < oldest->second) oldest = it;
    }
    marks->applied.erase(oldest);
  }
}

bool clipboard_echo_path(const std::string& path) {
  // A virtual clipboard mount outlives the two-minute echo window (a user may
  // paste much later), so every path under it is permanently ours. Without
  // this the local->remote poll would upload the mount back to the host.
  const std::string virtual_root = clipboard_virtual_directory();
  if (!virtual_root.empty()) {
    const std::string prefix = virtual_root + "/";
    if (path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0) {
      return true;
    }
  }
  const auto marks = clipboard_echoes();
  const uint64_t now = now_ms();
  std::lock_guard<std::mutex> lock(marks->mutex);
  const auto it = marks->applied.find(echo_path_key(path));
  if (it == marks->applied.end()) return false;
  return now >= it->second && now - it->second < kClipboardEchoTtlMs;
}

std::string clipboard_file_id(const clipboard_file& file) {
  uint64_t hash = 1469598103934665603ull;
  const std::string key =
      file.path + "|" + std::to_string(file.size);
  for (const unsigned char byte : key) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  char buffer[17] = {};
  std::snprintf(buffer, sizeof(buffer), "%016llx",
                static_cast<unsigned long long>(hash));
  return buffer;
}

void refresh_clipboard_files(
    const std::shared_ptr<clipboard_service>& service) {
  if (!service) return;
  const auto snapshot = clipboard_snapshot();
  bool expected = false;
  if (!snapshot->fetching.compare_exchange_strong(expected, true)) return;
  service->request_files([snapshot](std::vector<clipboard_file> files) {
    {
      std::lock_guard<std::mutex> lock(snapshot->mutex);
      snapshot->files = std::move(files);
      ++snapshot->version;
    }
    snapshot->fetching.store(false);
  });
}

/// Ask the experimental LocalSystem session service. Returns a JSON string
/// produced by the service, or an unavailable marker on other platforms.
std::string windows_session_command(const std::string& command) {
#ifdef _WIN32
  HANDLE pipe = CreateFileW(L"\\\\.\\pipe\\molink-session",
                            GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) {
    return R"({"ok":false,"available":false,"error":"service_unavailable"})";
  }
  DWORD mode = PIPE_READMODE_MESSAGE;
  SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
  DWORD written = 0;
  WriteFile(pipe, command.data(), static_cast<DWORD>(command.size()), &written,
            nullptr);
  char buffer[64 * 1024] = {};
  DWORD read = 0;
  ReadFile(pipe, buffer, sizeof(buffer) - 1, &read, nullptr);
  CloseHandle(pipe);
  return read > 0 ? std::string(buffer, read)
                  : R"({"ok":false,"error":"no_response"})";
#else
  (void)command;
  return R"({"ok":false,"available":false,"error":"windows_only"})";
#endif
}


using uvcpp::uvcpp_json;
using uvcpp::uvcpp_web_next;
using uvcpp::uvcpp_web_request;
using uvcpp::uvcpp_web_response;

bool origin_allowed(const std::string& origin) {
  // The shell loads the core's own origin, so only the Vite dev server needs
  // to be allowed here.
  return origin == "http://localhost:1420" ||
         origin == "http://127.0.0.1:1420";
}

std::string bearer_token(const uvcpp_web_request& req) {
  std::string supplied = req.header("X-MoLink-Token");
  if (!supplied.empty()) return supplied;
  const std::string authz = req.header("Authorization");
  const std::string prefix = "Bearer ";
  if (authz.compare(0, prefix.size(), prefix) == 0) {
    return authz.substr(prefix.size());
  }
  return std::string();
}

long current_pid() { return static_cast<long>(MOLINK_GETPID()); }

int effective_port(const api_deps& deps) {
  if (deps.bound_port) {
    const int port = deps.bound_port->load();
    if (port > 0) return port;
  }
  return deps.config.port;
}

/// Writes the shared 429/403 response for a throttled address. Returns true
/// when a response was sent.
bool reject_throttled(uvcpp_web_response& resp, int delay) {
  if (delay == 0) return false;
  uvcpp_json body;
  body["type"] = "error";
  if (delay < 0) {
    body["code"] = "ip_banned";
    body["message"] = "this address is blocked after repeated failures";
    resp.status(403).json(body);
  } else {
    body["code"] = "too_many_attempts";
    body["message"] = "too many failed attempts; wait before trying again";
    body["retry_after"] = delay;
    resp.set_header("Retry-After", std::to_string(delay));
    resp.status(429).json(body);
  }
  resp.end();
  return true;
}

uint64_t uptime_ms(uint64_t started) {
  const uint64_t now = now_ms();
  return now > started ? now - started : 0;
}

int stage_status_code(stage_error error) {
  switch (error) {
    case stage_error::unavailable:
      return 503;
    case stage_error::bad_request:
      return 400;
    case stage_error::not_found:
      return 404;
    case stage_error::conflict:
      return 409;
    case stage_error::too_large:
      return 413;
    case stage_error::io_error:
      return 500;
    case stage_error::apply_failed:
      return 409;
    case stage_error::none:
      break;
  }
  return 500;
}

const char* stage_error_code(stage_error error) {
  switch (error) {
    case stage_error::unavailable:
      return "stage_unavailable";
    case stage_error::bad_request:
      return "bad_stage_request";
    case stage_error::not_found:
      return "unknown_stage";
    case stage_error::conflict:
      return "offset_mismatch";
    case stage_error::too_large:
      return "file_too_large";
    case stage_error::io_error:
      return "stage_io_error";
    case stage_error::apply_failed:
      return "clipboard_apply_failed";
    case stage_error::none:
      break;
  }
  return "stage_failed";
}

void send_stage_error(uvcpp_web_response& resp, stage_error error) {
  uvcpp_json body;
  body["code"] = stage_error_code(error);
  resp.status(stage_status_code(error)).json(body);
  resp.end();
}

/// Strict decimal parse: no sign, no whitespace, no trailing characters.
bool parse_offset(const std::string& text, uint64_t& value) {
  if (text.empty()) return false;
  uint64_t out = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    const uint64_t digit = static_cast<uint64_t>(c - '0');
    if (out > (UINT64_MAX - digit) / 10) return false;
    out = out * 10 + digit;
  }
  value = out;
  return true;
}

uvcpp_json fs_entry_json(const fslist::entry& entry) {
  uvcpp_json item;
  item["name"] = entry.name;
  item["dir"] = entry.dir;
  item["size"] = entry.dir ? 0 : entry.size;
  item["mtime"] = entry.mtime;
  item["hidden"] = entry.hidden;
  item["system"] = entry.system;
  item["link"] = entry.link;
  if (!entry.error.empty()) item["error"] = entry.error;
  return item;
}

uvcpp_json fs_root_json(const fslist::root& root) {
  uvcpp_json item;
  item["name"] = root.name;
  item["path"] = root.path;
  item["kind"] = fslist::root_kind_name(root.kind);
  return item;
}

uvcpp_json fs_roots_json(const std::vector<fslist::root>& roots) {
  uvcpp_json array = uvcpp_json::array();
  for (const fslist::root& root : roots) array.push_back(fs_root_json(root));
  return array;
}

/// Resolve a path for the loopback-only local file routes. Unlike the remote
/// file channel this has always allowed any absolute path; the loopback peer
/// check is what keeps a remote page away from these routes.
std::optional<std::filesystem::path> local_absolute_path(
    const std::string& raw) {
  const std::string home = local_home_directory();
  if (raw.empty()) return path_from_utf8(home);
  const std::optional<std::string> normalized =
      fslist::normalize_browse_path(raw, home);
  if (!normalized) return std::nullopt;
  return path_from_utf8(*normalized);
}

/// The Host header without a port, lowercased. `[::1]:28418` becomes `::1`.
std::string host_header_name(const std::string& host) {
  std::string name = host;
  if (!name.empty() && name.front() == '[') {
    const std::size_t close = name.find(']');
    if (close != std::string::npos) name = name.substr(1, close - 1);
  } else {
    const std::size_t colon = name.find(':');
    // A second colon means this is a bare IPv6 literal, not host:port.
    if (colon != std::string::npos && name.find(':', colon + 1) ==
                                          std::string::npos) {
      name.resize(colon);
    }
  }
  for (char& ch : name) ch = static_cast<char>(std::tolower(
      static_cast<unsigned char>(ch)));
  return name;
}

/// /api/device answers without the per-launch client token so a desktop shell
/// can adopt a core it did not spawn (the moLinkSession service/helper owns
/// one in the user session). The handler has always been loopback-only; also
/// requiring a loopback Host closes the DNS-rebinding path that the client
/// token used to guard, where a remote page resolving to 127.0.0.1 would be
/// same-origin and could read the token.
bool loopback_host_header(const uvcpp_web_request& req) {
  const std::string host = req.header("Host");
  return !host.empty() && is_loopback_host(host_header_name(host));
}

}  // namespace

void register_http_routes(uvcpp::uvcpp_web_app& app, const api_deps& deps) {
  // Remote clipboard files are downloaded into the config directory first;
  // only after the whole selection arrived does it become the OS clipboard
  // (CF_HDROP / text/uri-list). The files stay behind because the clipboard
  // references them, not because staging needs them.
  std::string stage_directory = clipboard_cache_directory();
  if (stage_directory.empty()) {
    // No cache location (unusual, but a stripped-down service account may
    // have neither XDG_CACHE_HOME nor HOME): fall back to the config folder.
    const std::string config_dir = config_directory();
    if (!config_dir.empty()) {
      stage_directory =
          path_to_utf8(path_from_utf8(config_dir) / "clipboard-inbox");
    }
  }
  const auto staging = std::make_shared<clipboard_stage>(
      stage_directory,
      [clipboard = deps.clipboard](const std::vector<std::string>& paths) {
        if (!clipboard || !clipboard->available() ||
            !clipboard->set_files(paths)) {
          return false;
        }
        // These bytes came from the remote; the local file sync must not
        // offer them straight back.
        remember_clipboard_echoes(paths);
        return true;
      });

  // Virtual clipboard files: the viewer's OS clipboard names a read-only FUSE
  // mount instead of a downloaded copy. The page's loopback bridge relays each
  // range read to the host over its existing session. Without libfuse (or when
  // the mount fails) the page falls back to the download path.
  std::string virtual_directory = clipboard_virtual_directory();
  if (virtual_directory.empty()) {
    virtual_directory =
        stage_directory.empty()
            ? std::string()
            : path_to_utf8(path_from_utf8(stage_directory) / "mount");
  }
  std::function<bool(const std::vector<std::string>&)> virtual_apply =
      [clipboard = deps.clipboard](const std::vector<std::string>& paths) {
        if (!clipboard || !clipboard->available() ||
            !clipboard->set_files(paths)) {
          return false;
        }
        // The mount paths must not travel back to the remote as a fresh copy.
        remember_clipboard_echoes(paths);
        return true;
      };
  std::function<void()> virtual_clear = [clipboard = deps.clipboard] {
    if (clipboard && clipboard->available()) clipboard->set_text({});
  };
#if defined(_WIN32)
  // The Windows publisher installs an OLE virtual-file data object itself
  // (see windows_virtual_clipboard.cpp). There are no real paths to hand to
  // CF_HDROP, and releasing the object is the mount's job, so applying and
  // clearing the path list must stay side-effect free.
  virtual_apply = [](const std::vector<std::string>&) { return true; };
  virtual_clear = [] {};
#endif
  const auto virtual_clipboard_service = std::make_shared<virtual_clipboard>(
      virtual_directory,
      deps.clipboard != nullptr && deps.clipboard->available()
          ? platform_virtual_mount_factory()
          : virtual_mount_factory(),
      std::move(virtual_apply), std::move(virtual_clear),
      [&app](std::function<void()> fn) { app.post(std::move(fn)); });

  // ---- Remote access gate ---------------------------------------------
  // The listener is always on 0.0.0.0 for hot toggling; a connection accepted
  // before the switch was turned off is still rejected on every request.
  app.use([deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                 uvcpp_web_next next) {
    if (is_loopback_peer(req.peer_ip())) {
      next();
      return;
    }
    // A remote client is connected. This only marks its presence; the idle
    // timer still starts from /api/activity, so polling cannot keep a tab
    // unlocked forever.
    if (deps.lock) deps.lock->note_remote();
    const app_settings settings =
        deps.auth ? deps.auth->settings() : app_settings{};
    if (settings.allow_remote_access) {
      next();
      return;
    }
    uvcpp_json body;
    body["type"] = "error";
    body["code"] = "remote_access_disabled";
    body["message"] = "LAN access is disabled; enable it in Security";
    resp.status(403).json(body);
    resp.end();
  });

  // ---- Loopback shell gate --------------------------------------------
  // A local browser is also "outside the shell". When allow_web_client is off,
  // the UI/state/login/device paths require the per-launch client token that
  // only the shell receives on its ready line. WebSocket upgrades are exempt
  // because browser WebSocket cannot send a custom header; they still require
  // a code or session token.
  app.use([deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                 uvcpp_web_next next) {
    if (!is_loopback_peer(req.peer_ip())) {
      next();
      return;
    }
    const app_settings settings =
        deps.auth ? deps.auth->settings() : app_settings{};
    if (settings.allow_web_client) {
      next();
      return;
    }
    const std::string& path = req.path();
    // Do not gate hashed assets: the browser fetches them without custom
    // headers. Gating the HTML entry point plus the state/login APIs is
    // enough; a browser cannot use the UI without the HTML and cannot obtain a
    // session token without the client token.
    // /api/transports is the capability advertisement peer cores fetch before
    // they open a data channel; gating it behind the shell token would make
    // core-to-core transport negotiation impossible. It exposes only ports
    // and availability, which /healthz already implies.
    // /api/device stays open on loopback so a shell can adopt a core started
    // by the service/helper; the handler itself rejects non-loopback peers and
    // non-loopback Host headers, so this does not undo the rebinding guard.
    const bool gated = path == "/" || path == "/index.html" ||
                       path == "/api/state" || path == "/api/login";
    const std::string* query_client = req.query("client");
    const bool token_ok =
        req.header("X-MoLink-Client") == deps.client_token ||
        (query_client != nullptr && *query_client == deps.client_token);
    if (!gated || token_ok) {
      next();
      return;
    }
    uvcpp_json body;
    body["type"] = "error";
    body["code"] = "client_token_required";
    body["message"] =
        "the web UI is only available through the desktop shell";
    resp.status(403).json(body);
    resp.end();
  });

  // ---- Remote browser gate --------------------------------------------
  // The desktop shell is loopback and always allowed. With allow_web_client
  // off, a non-loopback peer cannot load the UI, the login page, or the state
  // probe it needs; the remote-desktop WebSocket and authenticated APIs remain
  // available to moLink clients that know the code.
  app.use([deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                 uvcpp_web_next next) {
    if (is_loopback_peer(req.peer_ip())) {
      next();
      return;
    }
    const app_settings settings =
        deps.auth ? deps.auth->settings() : app_settings{};
    if (settings.allow_web_client && settings.allow_remote_access) {
      next();
      return;
    }
    const std::string& path = req.path();
    const bool websocket =
        req.header("Upgrade").find("websocket") != std::string::npos;
    const bool api = path.compare(0, 5, "/api/") == 0;
    const bool ui_or_login =
        path == "/healthz" ? false
                           : ((!api && !websocket) || path == "/api/state" ||
                              path == "/api/login");
    if (!ui_or_login) {
      next();
      return;
    }
    uvcpp_json body;
    body["type"] = "error";
    body["code"] = "web_client_disabled";
    body["message"] = "remote web access is disabled; enable it in Security";
    resp.status(403).json(body);
    resp.end();
  });

  // ---- CORS ------------------------------------------------------------
  app.use([](uvcpp_web_request& req, uvcpp_web_response& resp,
             uvcpp_web_next next) {
    const std::string origin = req.header("Origin");
    if (!origin.empty() && origin_allowed(origin)) {
      resp.set_header("Access-Control-Allow-Origin", origin);
      resp.set_header("Vary", "Origin");
      resp.set_header("Access-Control-Allow-Methods",
                      "GET, POST, PUT, DELETE, OPTIONS");
      resp.set_header("Access-Control-Allow-Headers",
                      "Authorization, Content-Type, X-MoLink-Token, "
                      "X-MoLink-Client");
      resp.set_header("Access-Control-Max-Age", "600");
    }
    resp.set_header("X-Content-Type-Options", "nosniff");
    if (req.method() == uvcpp::http_method::HTTP_OPTIONS) {
      resp.status(204).end();
      return;
    }
    next();
  });

  // ---- Idle lock -------------------------------------------------------
  // Only remote browsers lock. The local shell/browser is the person sitting
  // at the machine and stays usable; while a remote tab is locked, only the
  // state probe and the login/unlock endpoint remain reachable for it.
  app.use([deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                 uvcpp_web_next next) {
    if (!deps.lock || is_loopback_peer(req.peer_ip())) {
      next();
      return;
    }
    const std::string& path = req.path();
    const bool is_api = path.compare(0, 5, "/api/") == 0;
    const bool allowed = path == "/api/state" || path == "/api/login" ||
                         path == "/api/health" || path == "/api/transports";
    if (is_api && !allowed && deps.lock->locked()) {
      uvcpp_json body;
      body["type"] = "error";
      body["code"] = "locked";
      body["message"] = "the UI is locked; enter the verification code";
      resp.status(423).json(body);
      resp.end();
      return;
    }
    next();
  });

  // ---- Token guard for /api/* -----------------------------------------
  // /healthz and /api/health stay open: the shell's readiness probe and
  // monitoring need them, and they expose nothing beyond version and uptime.
  app.use([deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                 uvcpp_web_next next) {
    const std::string& path = req.path();
    const bool is_api = path.compare(0, 5, "/api/") == 0;
    // /api/device is loopback-only and self-checks the peer address.
    if (!is_api || path == "/api/health" || path == "/api/device" ||
        path == "/api/state" || path == "/api/login" ||
        path == "/api/transports" || path == "/api/ui-preferences") {
      next();
      return;
    }
    // The bearer value is a session token issued by /api/login (or the local
    // shell's stable token). Verification codes are only accepted at login.
    if (deps.lock &&
        reject_throttled(resp, deps.lock->attempt_delay(req.peer_ip()))) {
      return;
    }
    const std::string supplied = bearer_token(req);
    if (deps.auth && deps.auth->valid_session(supplied)) {
      if (deps.lock) deps.lock->record_success(req.peer_ip());
      next();
      return;
    }
    if (!supplied.empty() && deps.lock &&
        reject_throttled(resp, deps.lock->record_failure(req.peer_ip()))) {
      return;
    }
    uvcpp_json body;
    body["type"] = "error";
    body["code"] = "unauthorized";
    body["message"] = "missing or invalid bearer token";
    resp.status(401).set_header("WWW-Authenticate", "Bearer").json(body);
    resp.end();
  });

  auto health = [deps](uvcpp_web_request&, uvcpp_web_response& resp,
                       uvcpp_web_next) {
    uvcpp_json j;
    j["status"] = "ok";
    j["service"] = MOLINK_CORE_SERVICE;
    j["version"] = MOLINK_VERSION;
    j["protocol"] = 1;
    j["pid"] = current_pid();
    j["uptime_ms"] = uptime_ms(deps.started_ms);
    j["sessions"] = deps.registry->size();
    resp.set_header("Cache-Control", "no-store").json(j);
    resp.end();
  };
  app.get("/healthz", health);
  app.get("/api/health", health);

  app.get("/api/state",
          [deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                 uvcpp_web_next) {
            uvcpp_json j;
            j["service"] = MOLINK_CORE_SERVICE;
            j["product"] = MOLINK_PRODUCT_NAME;
            j["version"] = MOLINK_VERSION;
            j["protocol"] = 1;
            j["device_id"] = deps.device_id;
            const bool local = is_loopback_peer(req.peer_ip());
            j["local"] = local;
            const app_settings current_settings =
                deps.auth ? deps.auth->settings() : app_settings{};
            j["device_name"] = current_settings.device_name;
            j["allow_access_requests"] = current_settings.allow_access_requests;
            j["allow_remote_access"] = current_settings.allow_remote_access;
            j["avatar"] = current_settings.avatar;
            j["platform"] = device_platform();
            if (deps.server) {
              const server_status status = deps.server->status();
              uvcpp_json server;
              server["url"] = effective_server_url(current_settings);
              server["custom_url"] = current_settings.server_url;
              server["default_url"] = default_server_url();
              server["enabled"] = current_settings.server_enabled;
              server["using_default"] =
                  current_settings.server_enabled &&
                  current_settings.server_url.empty();
              server["registered"] = status.registered;
              server["online"] = status.online;
              server["device_id"] = status.device_id;
              server["display_id"] = status.display_id;
              server["domain"] = status.domain;
              server["domain_valid"] = status.domain_valid;
              server["last_error"] = status.last_error;
              j["server"] = server;
            }
            if (local && deps.access_requests) {
              j["access_requests"] = uvcpp_json::array();
              for (const access_request_info& request :
                   deps.access_requests->list()) {
                uvcpp_json item;
                item["id"] = request.id;
                item["peer_ip"] = request.peer_ip;
                item["name"] = request.name;
                item["client"] = request.client;
                item["created_ms"] = request.created_ms;
                j["access_requests"].push_back(std::move(item));
              }
            }
            // The local UI never reports the lock: it is exempt from the gate,
            // and a remote tab going idle must not show a lock screen here.
            j["locked"] =
                deps.lock != nullptr && deps.lock->locked() && !local;
            j["lock_timeout"] =
                deps.lock != nullptr ? deps.lock->timeout_seconds() : 0;
            j["open_sessions"] =
                deps.lock != nullptr ? deps.lock->open_sessions() : 0;
            const int configured_tcp =
                deps.network ? deps.network->tcp_port.load() : deps.config.port;
            const int configured_udp =
                deps.network ? deps.network->udp_port.load() : deps.config.udp_port;
            const int configured_h3 =
                deps.network ? deps.network->http3_port.load()
                             : deps.config.http3_port;
            const int configured_quic =
                deps.network ? deps.network->quic_port.load()
                             : deps.config.quic_port;
            const int configured_tcp_data =
                deps.network ? deps.network->tcp_data_port.load()
                             : deps.config.tcp_data_port;
            const int bound = effective_port(deps);
            j["host"] = deps.config.host;
            j["port"] = bound;
            j["bound_port"] = bound;
            j["tcp_port"] = configured_tcp;
            j["udp_port"] = configured_udp;
            j["http3_port"] = configured_h3;
            j["quic_port"] = configured_quic;
            j["tcp_data_port"] = configured_tcp_data;
            j["restart_required"] =
                configured_tcp != bound ||
                configured_udp != deps.config.udp_port ||
                configured_h3 != deps.config.http3_port ||
                configured_quic != deps.config.quic_port ||
                configured_tcp_data != deps.config.tcp_data_port;
            resp.set_header("Cache-Control", "no-store").json(j);
            resp.end();
          });

  // Transport negotiation catalogue. The browser always has WebSocket; a
  // desktop peer can read this endpoint (or the matching `welcome` fields) and
  // pick the first transport it supports. Reserved entries are advertised with
  // available=false so clients do not try a port this build does not listen on.
  app.get("/api/transports",
          [deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                 uvcpp_web_next) {
            const int web_port = effective_port(deps);
            const auto port_of = [&deps](std::atomic<int> network_settings::*field,
                                         int fallback) {
              return deps.network ? (deps.network.get()->*field).load()
                                  : fallback;
            };
            const int http3_port =
                port_of(&network_settings::http3_port, deps.config.http3_port);
            const int quic_port =
                port_of(&network_settings::quic_port, deps.config.quic_port);
            const int tcp_data_port = port_of(
                &network_settings::tcp_data_port, deps.config.tcp_data_port);
            const int udp_port =
                port_of(&network_settings::udp_port, deps.config.udp_port);

            uvcpp_json j;
            j["current"] = "websocket";
            j["preferred"] = uvcpp_json::array();
            for (const char* name :
                 {"quic", "http3", "tcp", "http2", "websocket", "udp"}) {
              j["preferred"].push_back(name);
            }
            j["web"]["path"] = "/ws/session";
            // Public material for cert pinning on the data channels. The
            // fingerprint is checked by the client before the PEM becomes a
            // trusted CA; exposing a public certificate is harmless.
            if (!deps.cert_fingerprint.empty()) {
              j["tls_fingerprint"] = deps.cert_fingerprint;
            }
            if (!deps.tls_cert_pem.empty()) {
              j["tls_cert"] = deps.tls_cert_pem;
            }
            j["ports"]["web"] = web_port;
            j["ports"]["http3"] = http3_port;
            j["ports"]["quic"] = quic_port;
            j["ports"]["tcp_data"] = tcp_data_port;
            j["ports"]["udp_data"] = udp_port;
            j["transports"] = uvcpp_json::array();
            const auto add = [&j](const char* name, const char* role,
                                  bool available, bool secure, int port,
                                  const char* note) {
              uvcpp_json item;
              item["name"] = name;
              item["role"] = role;
              item["available"] = available;
              item["secure"] = secure;
              item["port"] = port;
              item["note"] = note;
              j["transports"].push_back(std::move(item));
            };
            add("websocket", "web", true, false, web_port,
                "browser baseline; /ws/session");
            add("http2", "data", deps.http2_available, false, web_port,
                "WebSocket over HTTP/2 (RFC 8441) on the web port");
            add("http3", "data",
                deps.http3_available && deps.http3_available->load(), true,
                http3_port, "WebSocket over HTTP/3 (RFC 9220)");
            // While remote access is off the listener stays on loopback, so
            // only a loopback caller can actually reach it.
            const bool quic_reachable =
                deps.quic_available && deps.quic_available->load() &&
                (is_loopback_peer(req.peer_ip()) ||
                 (deps.auth && deps.auth->settings().allow_remote_access));
            add("quic", "data", quic_reachable, true, quic_port,
                "MOLK session stream over QUIC");
            add("tcp", "data",
                deps.tcp_available && deps.tcp_available->load(), true,
                tcp_data_port, "MOLK session stream over TLS/TCP");
            add("udp", "data", false, false, udp_port,
                "direct data channel (planned)");
            resp.set_header("Cache-Control", "no-store").json(j);
            resp.end();
          });

  app.post("/api/login",
           [deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                  uvcpp_web_next) {
             if (!deps.lock) {
               resp.status(503).json_str(R"({"error":"locking unavailable"})");
               resp.end();
               return;
             }
             const std::string peer = req.peer_ip();
             if (reject_throttled(resp, deps.lock->attempt_delay(peer))) return;
             // The second factor has its own failure budget; a locked TOTP
             // budget must also stop the request before the password check.
             if (reject_throttled(
                     resp, deps.lock->attempt_delay(peer, "totp"))) {
               return;
             }
             // The six-digit code travels in a header or a small JSON body.
             // (An old client sends neither, which is fine until 2FA is on.)
             std::string totp = req.header("X-MoLink-TOTP");
             if (totp.empty() && req.body_size() > 0) {
               try {
                 const uvcpp_json body =
                     uvcpp_json::parse(req.body_str());
                 if (body.is_object()) {
                   const auto it = body.find("totp");
                   if (it != body.end() && it->is_string()) {
                     totp = it->get<std::string>();
                   }
                 }
               } catch (...) {
                 // A malformed body is treated as "no second factor"; the
                 // normal missing/invalid replies below cover it.
               }
             }
             const login_result result =
                 deps.auth ? deps.auth->login(bearer_token(req), totp,
                                              /*consume=*/true)
                           : login_result{};
             if (result.totp_required) {
               uvcpp_json body;
               body["type"] = "error";
               body["code"] = "totp_required";
               body["message"] = "a six-digit two-factor code is required";
               resp.status(401).json(body);
               resp.end();
               return;
             }
             if (result.totp_invalid) {
               if (reject_throttled(
                       resp, deps.lock->record_failure(peer, "totp"))) {
                 return;
               }
               uvcpp_json body;
               body["type"] = "error";
               body["code"] = "invalid_totp";
               body["message"] = "the six-digit code is not valid";
               resp.status(401).json(body);
               resp.end();
               return;
             }
             if (!result.ok) {
               if (reject_throttled(resp, deps.lock->record_failure(peer))) {
                 return;
               }
               uvcpp_json body;
               body["type"] = "error";
               body["code"] = "invalid_token";
               body["message"] = "the verification code is not valid";
               resp.status(401).json(body);
               resp.end();
               return;
             }
             deps.lock->unlock();
             deps.lock->record_success(peer);
             deps.lock->record_success(peer, "totp");
             uvcpp_json j;
             j["ok"] = true;
             j["session_token"] = deps.auth->create_session();
             j["device_id"] = deps.device_id;
             j["service"] = MOLINK_CORE_SERVICE;
             j["version"] = MOLINK_VERSION;
             j["protocol"] = 1;
             j["local"] = is_loopback_peer(req.peer_ip());
             j["lock_timeout"] = deps.lock->timeout_seconds();
             resp.set_header("Cache-Control", "no-store").json(j);
             resp.end();
           });

  app.post("/api/activity",
           [deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                  uvcpp_web_next) {
             if (deps.lock) deps.lock->touch(!is_loopback_peer(req.peer_ip()));
             resp.status(204).end();
           });

  // Phase-2 directory configuration and status (local host UI only).
  auto server_json = [deps]() {
    const app_settings set = deps.auth ? deps.auth->settings() : app_settings{};
    const server_status status =
        deps.server ? deps.server->status() : server_status{};
    uvcpp_json j;
    j["url"] = effective_server_url(set);
    j["custom_url"] = set.server_url;
    j["default_url"] = default_server_url();
    j["invite"] = set.server_invite;
    j["ca"] = set.server_ca;
    j["enabled"] = set.server_enabled;
    j["using_default"] = set.server_enabled && set.server_url.empty();
    j["registered"] = status.registered;
    j["online"] = status.online;
    j["device_id"] = status.device_id;
    j["display_id"] = status.display_id;
    j["domain"] = status.domain;
    j["domain_valid"] = status.domain_valid;
    j["last_error"] = status.last_error;
    return j;
  };
  app.get("/api/server",
          [deps, server_json](uvcpp_web_request& req, uvcpp_web_response& resp,
                              uvcpp_web_next) {
            if (!is_loopback_peer(req.peer_ip()) || !deps.auth) {
              resp.status(403).json_str(R"({"code":"loopback_only"})");
              resp.end();
              return;
            }
            resp.set_header("Cache-Control", "no-store").json(server_json());
            resp.end();
          });
  app.post("/api/server",
           [deps, server_json](uvcpp_web_request& req, uvcpp_web_response& resp,
                               uvcpp_web_next) {
             if (!is_loopback_peer(req.peer_ip()) || !deps.auth) {
               resp.status(403).json_str(R"({"code":"loopback_only"})");
               resp.end();
               return;
             }
             const app_settings before = deps.auth->settings();
             const std::string* url = req.query("url");
             const std::string* invite = req.query("invite");
             const std::string* ca = req.query("ca");
             const std::string* enabled = req.query("enabled");
             bool server_enabled = before.server_enabled;
             if (enabled != nullptr) {
               server_enabled =
                   *enabled == "1" || *enabled == "true" || *enabled == "yes";
             }
             std::string error;
             if (!deps.auth->set_server(
                     url != nullptr ? *url : before.server_url,
                     invite != nullptr ? *invite : before.server_invite,
                     ca != nullptr ? *ca : before.server_ca, server_enabled,
                     error)) {
               uvcpp_json body;
               body["code"] = "invalid_server";
               body["message"] = error;
               resp.status(400).json(body);
               resp.end();
               return;
             }
             if (deps.server) deps.server->reconfigure();
             resp.set_header("Cache-Control", "no-store").json(server_json());
             resp.end();
           });

  // Device lookups through this core's directory connection. The browser used
  // to call /v1/devices directly, which required it (not the core) to trust the
  // directory's CA; with a private/Origin certificate that failed and the UI
  // reported "could not reach the ID server" even though ICE sessions worked.
  auto directory_lookup = [deps](uvcpp_web_request& req,
                                 uvcpp_web_response& resp,
                                 const std::string& query) {
    (void)req;
    if (!deps.server) {
      resp.status(503).json_str(R"({"code":"directory_unavailable"})");
      resp.end();
      return;
    }
    std::string body;
    std::string error;
    int status = 0;
    if (!deps.server->directory_lookup(query, body, status, error)) {
      uvcpp_json j;
      j["type"] = "error";
      j["code"] = "directory_unreachable";
      j["message"] = error;
      resp.status(502).json(j);
      resp.end();
      return;
    }
    resp.set_header("Cache-Control", "no-store");
    if (status == 200) {
      resp.json_str(body);
    } else if (status == 404) {
      resp.status(404).json_str(
          R"({"type":"error","code":"unknown_device","message":"unknown device"})");
    } else if (status == 429) {
      resp.status(429).json_str(
          R"({"type":"error","code":"rate_limited","message":"the directory rate-limited this lookup"})");
    } else {
      uvcpp_json j;
      j["type"] = "error";
      j["code"] = "directory_error";
      j["message"] = body.empty() ? error : body;
      resp.status(502).json(j);
    }
    resp.end();
  };
  app.get("/api/directory/device",
          [directory_lookup](uvcpp_web_request& req, uvcpp_web_response& resp,
                             uvcpp_web_next) {
            const std::string* id = req.query("id");
            if (id == nullptr || id->empty()) {
              resp.status(400).json_str(R"({"code":"bad_request"})");
              resp.end();
              return;
            }
            directory_lookup(req, resp, "id=" + url_encode(*id));
          });
  app.get("/api/directory/devices",
          [directory_lookup](uvcpp_web_request& req, uvcpp_web_response& resp,
                             uvcpp_web_next) {
            const std::string* ids = req.query("ids");
            if (ids == nullptr || ids->empty()) {
              resp.status(400).json_str(R"({"code":"bad_request"})");
              resp.end();
              return;
            }
            directory_lookup(req, resp, "ids=" + url_encode(*ids));
          });

  // Chat between the person at this machine and a controlling client. The
  // local UI polls for new messages; the session websocket carries the other
  // side. Both use the same shared log.
  app.get("/api/chat",
          [deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                 uvcpp_web_next) {
            if (!is_loopback_peer(req.peer_ip()) || !deps.chat) {
              resp.status(403).json_str(R"({"code":"loopback_only"})");
              resp.end();
              return;
            }
            uint64_t since = 0;
            if (const std::string* value = req.query("since")) {
              try {
                since = std::stoull(*value);
              } catch (...) {
                since = 0;
              }
            }
            uvcpp_json list = uvcpp_json::array();
            for (const chat_message& message : deps.chat->history(since)) {
              uvcpp_json item;
              item["id"] = message.id;
              item["from"] = message.from;
              item["sender_name"] = message.sender_name;
              item["sender_id"] = message.sender_id;
              item["sender_session"] = message.sender_session;
              item["text"] = message.text;
              item["ts"] = message.ts_ms;
              list.push_back(std::move(item));
            }
            uvcpp_json j;
            j["messages"] = std::move(list);
            j["cursor"] = deps.chat->cursor();
            resp.set_header("Cache-Control", "no-store").json(j);
            resp.end();
          });

  app.post("/api/chat",
           [deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                  uvcpp_web_next) {
             if (!is_loopback_peer(req.peer_ip()) || !deps.chat) {
               resp.status(403).json_str(R"({"code":"loopback_only"})");
               resp.end();
               return;
             }
             const std::string* value = req.query("text");
             const std::string text = value != nullptr ? *value : std::string();
             if (text.empty() || text.size() > 4096) {
               resp.status(400).json_str(
                   R"({"code":"bad_chat","message":"text must be 1..4096 bytes"})");
               resp.end();
               return;
             }
             const std::string host_name =
                 deps.auth ? deps.auth->settings().device_name : std::string();
             deps.chat->post("host", text, host_name, deps.device_id, 0);
             resp.status(204).end();
           });

  // The local pane lists a folder the browser itself cannot read. Loopback
  // only: a page served by a remote core must never enumerate this machine's
  // filesystem. The page calls this only when /api/state reported
  // `local: true`, so the listing describes the browser's own machine.
  app.get("/api/fs/list",
          [](uvcpp_web_request& req, uvcpp_web_response& resp,
             uvcpp_web_next) {
            if (!is_loopback_peer(req.peer_ip())) {
              resp.status(403).json_str(R"({"code":"loopback_only"})");
              resp.end();
              return;
            }
            const std::string home = local_home_directory();
            const std::vector<fslist::root> roots =
                fslist::system_roots(home);
            uvcpp_json roots_json = fs_roots_json(roots);
            uvcpp_json entries = uvcpp_json::array();
            const bool want_roots = req.query("roots") != nullptr ||
                                    (req.query("path") != nullptr &&
                                     req.query("path")->empty());
            std::string path;
            std::string parent;
            if (want_roots) {
              // Volume list: every root is a directory row with an absolute
              // `path`, exactly like the remote channel's roots view.
              for (const fslist::root& root : roots) {
                uvcpp_json item = fs_root_json(root);
                item["dir"] = true;
                item["size"] = 0;
                item["mtime"] = 0;
                item["hidden"] = false;
                item["system"] = false;
                item["link"] = false;
                item["root"] = true;
                entries.push_back(std::move(item));
              }
            } else {
              const std::string* requested = req.query("path");
              const std::optional<fs::path> target =
                  local_absolute_path(requested != nullptr ? *requested : "");
              if (!target) {
                resp.status(400).json_str(
                    R"({"code":"bad_path","message":"invalid path"})");
                resp.end();
                return;
              }
              std::string why;
              const std::optional<fslist::listing> listed =
                  fslist::list_directory(fslist::native_path(*target), why);
              if (!listed) {
                uvcpp_json error;
                error["code"] = "bad_path";
                error["message"] = why;
                resp.status(400).json(error);
                resp.end();
                return;
              }
              path = listed->path;
              parent = listed->parent;
              for (const fslist::entry& entry : listed->entries) {
                entries.push_back(fs_entry_json(entry));
              }
            }
            uvcpp_json j;
            j["path"] = path;
            j["root"] = home;
            j["parent"] = parent;
            j["roots"] = std::move(roots_json);
            j["entries"] = std::move(entries);
            resp.set_header("Cache-Control", "no-store").json(j);
            resp.end();
          });

  // The local pane reads and writes files through these routes when the page
  // is served by the local core (the desktop shell). The browser's File System
  // Access API does not exist there, so without this "send from any folder"
  // and "receive into the current folder" cannot work. Loopback only, like
  // /api/fs/list: a remote page must never read or write this machine.
  app.get("/api/fs/read",
          [](uvcpp_web_request& req, uvcpp_web_response& resp,
             uvcpp_web_next) {
            if (!is_loopback_peer(req.peer_ip())) {
              resp.status(403).json_str(R"({"code":"loopback_only"})");
              resp.end();
              return;
            }
            const std::string* requested = req.query("path");
            const std::optional<fs::path> target = local_absolute_path(
                requested != nullptr ? *requested : "");
            if (!target) {
              resp.status(400).json_str(R"({"code":"bad_path"})");
              resp.end();
              return;
            }
            std::error_code ec;
            const fs::path file = *target;
            if (!fs::is_regular_file(file, ec) || ec) {
              resp.status(404).json_str(R"({"code":"missing_file"})");
              resp.end();
              return;
            }
            const uint64_t size = fs::file_size(file, ec);
            if (ec) {
              resp.status(404).json_str(R"({"code":"missing_file"})");
              resp.end();
              return;
            }
            constexpr uint64_t kMaxReadBytes = 8ull * 1024ull * 1024ull;
            uint64_t offset = 0;
            uint64_t length = 0;
            const std::string* offset_query = req.query("offset");
            const std::string* length_query = req.query("length");
            if (offset_query != nullptr &&
                !parse_offset(*offset_query, offset)) {
              resp.status(400).json_str(R"({"code":"bad_offset"})");
              resp.end();
              return;
            }
            if (length_query == nullptr ||
                !parse_offset(*length_query, length) ||
                length > kMaxReadBytes) {
              resp.status(400).json_str(R"({"code":"bad_length"})");
              resp.end();
              return;
            }
            if (offset > size) {
              resp.status(416).json_str(
                  R"({"code":"offset_out_of_range"})");
              resp.end();
              return;
            }
            const uint64_t take = (std::min)(length, size - offset);
            std::ifstream in(file, std::ios::binary);
            if (!in) {
              resp.status(404).json_str(R"({"code":"missing_file"})");
              resp.end();
              return;
            }
            std::string data;
            if (take != 0) {
              in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
              if (!in) {
                resp.status(416).json_str(
                    R"({"code":"offset_out_of_range"})");
                resp.end();
                return;
              }
              data.resize(static_cast<std::size_t>(take));
              in.read(data.data(), static_cast<std::streamsize>(take));
              const std::streamsize got = in.gcount();
              data.resize(got > 0 ? static_cast<std::size_t>(got) : 0);
            }
            resp.set_header("Cache-Control", "no-store")
                .body(data, "application/octet-stream")
                .end();
          });

  app.post("/api/fs/write",
           [](uvcpp_web_request& req, uvcpp_web_response& resp,
              uvcpp_web_next) {
             if (!is_loopback_peer(req.peer_ip())) {
               resp.status(403).json_str(R"({"code":"loopback_only"})");
               resp.end();
               return;
             }
             const std::string* requested = req.query("path");
             const std::optional<fs::path> target = local_absolute_path(
                 requested != nullptr ? *requested : "");
             const std::string* offset_query = req.query("offset");
             uint64_t offset = 0;
             if (!target || offset_query == nullptr ||
                 !parse_offset(*offset_query, offset)) {
               resp.status(400).json_str(R"({"code":"bad_write"})");
               resp.end();
               return;
             }
             constexpr std::size_t kMaxWriteBytes = 8 * 1024 * 1024;
             if (req.body_size() > kMaxWriteBytes) {
               resp.status(413).json_str(R"({"code":"too_large"})");
               resp.end();
               return;
             }
             const std::string* truncate_query = req.query("truncate");
             const bool truncate =
                 truncate_query != nullptr && *truncate_query == "1";
             std::error_code ec;
             const fs::path file = *target;
             if (!file.parent_path().empty()) {
               fs::create_directories(file.parent_path(), ec);
               if (ec) {
                 resp.status(400).json_str(R"({"code":"bad_write"})");
                 resp.end();
                 return;
               }
             }
             std::fstream out;
             if (offset == 0 && truncate) {
               out.open(file, std::ios::binary | std::ios::out |
                                  std::ios::trunc);
             } else {
               out.open(file, std::ios::binary | std::ios::in |
                                  std::ios::out);
             }
             if (!out) {
               resp.status(404).json_str(R"({"code":"missing_file"})");
               resp.end();
               return;
             }
             out.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
             if (!out) {
               resp.status(416).json_str(
                   R"({"code":"offset_out_of_range"})");
               resp.end();
               return;
             }
             out.write(req.body_data(),
                       static_cast<std::streamsize>(req.body_size()));
             out.flush();
             if (!out) {
               resp.status(500).json_str(R"({"code":"write_failed"})");
               resp.end();
               return;
             }
             uvcpp_json j;
             j["ok"] = true;
             j["received"] =
                 offset + static_cast<uint64_t>(req.body_size());
             resp.set_header("Cache-Control", "no-store").json(j);
             resp.end();
           });

  app.post("/api/fs/move",
           [](uvcpp_web_request& req, uvcpp_web_response& resp,
              uvcpp_web_next) {
             if (!is_loopback_peer(req.peer_ip())) {
               resp.status(403).json_str(R"({"code":"loopback_only"})");
               resp.end();
               return;
             }
             uvcpp_json body;
             try {
               body = uvcpp_json::parse(req.body_str());
             } catch (...) {
               resp.status(400).json_str(R"({"code":"bad_move"})");
               resp.end();
               return;
             }
             if (!body.is_object() || !body.contains("from") ||
                 !body.contains("to") || !body["from"].is_string() ||
                 !body["to"].is_string()) {
               resp.status(400).json_str(R"({"code":"bad_move"})");
               resp.end();
               return;
             }
             const std::optional<fs::path> from =
                 local_absolute_path(body["from"].get<std::string>());
             const std::optional<fs::path> to =
                 local_absolute_path(body["to"].get<std::string>());
             if (!from || !to) {
               resp.status(400).json_str(R"({"code":"bad_move"})");
               resp.end();
               return;
             }
             std::error_code ec;
             if (!fs::is_regular_file(*from, ec) || ec) {
               resp.status(404).json_str(R"({"code":"missing_file"})");
               resp.end();
               return;
             }
             if (!to->parent_path().empty()) {
               fs::create_directories(to->parent_path(), ec);
               if (ec) {
                 resp.status(400).json_str(R"({"code":"bad_move"})");
                 resp.end();
                 return;
               }
             }
             const std::string final_path =
                 fslist::unique_destination(fslist::native_path(*to));
             fs::rename(*from, path_from_utf8(final_path), ec);
             if (ec) {
               resp.status(500).json_str(R"({"code":"move_failed"})");
               resp.end();
               return;
             }
             uvcpp_json j;
             j["ok"] = true;
             j["path"] = final_path;
             resp.set_header("Cache-Control", "no-store").json(j);
             resp.end();
           });

  app.post("/api/fs/mkdir",
           [](uvcpp_web_request& req, uvcpp_web_response& resp,
              uvcpp_web_next) {
             if (!is_loopback_peer(req.peer_ip())) {
               resp.status(403).json_str(R"({"code":"loopback_only"})");
               resp.end();
               return;
             }
             uvcpp_json body;
             try {
               body = uvcpp_json::parse(req.body_str());
             } catch (...) {
               resp.status(400).json_str(R"({"code":"bad_mkdir"})");
               resp.end();
               return;
             }
             if (!body.is_object() || !body.contains("path") ||
                 !body["path"].is_string()) {
               resp.status(400).json_str(R"({"code":"bad_mkdir"})");
               resp.end();
               return;
             }
             const std::optional<fs::path> target =
                 local_absolute_path(body["path"].get<std::string>());
             if (!target) {
               resp.status(400).json_str(R"({"code":"bad_mkdir"})");
               resp.end();
               return;
             }
             std::error_code ec;
             fs::create_directories(*target, ec);
             if (ec || !fs::is_directory(*target, ec)) {
               resp.status(500).json_str(R"({"code":"mkdir_failed"})");
               resp.end();
               return;
             }
             uvcpp_json j;
             j["ok"] = true;
             j["path"] = fslist::native_path(*target);
             resp.set_header("Cache-Control", "no-store").json(j);
             resp.end();
           });

  app.post("/api/fs/remove",
           [](uvcpp_web_request& req, uvcpp_web_response& resp,
              uvcpp_web_next) {
             if (!is_loopback_peer(req.peer_ip())) {
               resp.status(403).json_str(R"({"code":"loopback_only"})");
               resp.end();
               return;
             }
             uvcpp_json body;
             try {
               body = uvcpp_json::parse(req.body_str());
             } catch (...) {
               resp.status(400).json_str(R"({"code":"bad_remove"})");
               resp.end();
               return;
             }
             if (!body.is_object() || !body.contains("path") ||
                 !body["path"].is_string()) {
               resp.status(400).json_str(R"({"code":"bad_remove"})");
               resp.end();
               return;
             }
             const std::optional<fs::path> target =
                 local_absolute_path(body["path"].get<std::string>());
             if (!target) {
               resp.status(400).json_str(R"({"code":"bad_remove"})");
               resp.end();
               return;
             }
             std::error_code ec;
             if (!fs::is_regular_file(*target, ec) || ec) {
               // Already gone is success: the caller is cleaning up a staged
               // download and only wants the file not to remain.
               uvcpp_json j;
               j["ok"] = true;
               resp.set_header("Cache-Control", "no-store").json(j);
               resp.end();
               return;
             }
             fs::remove(*target, ec);
             uvcpp_json j;
             j["ok"] = !ec;
             if (ec) j["message"] = "could not remove the file";
             resp.set_header("Cache-Control", "no-store").json(j);
             resp.end();
           });

  // The local shell watches its own clipboard for files. The page cannot
  // touch the OS clipboard's file list, so it polls this snapshot and then
  // uploads the contents to the remote session. Loopback only: a remote
  // browser must never receive the host machine's clipboard files.
  app.get("/api/clipboard-files",
          [deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                 uvcpp_web_next) {
            if (!is_loopback_peer(req.peer_ip()) || !deps.clipboard) {
              resp.status(403).json_str(R"({"code":"loopback_only"})");
              resp.end();
              return;
            }
            refresh_clipboard_files(deps.clipboard);
            const auto snapshot = clipboard_snapshot();
            std::vector<clipboard_file> files;
            uint64_t version = 0;
            {
              std::lock_guard<std::mutex> lock(snapshot->mutex);
              files = snapshot->files;
              version = snapshot->version;
            }
            uvcpp_json list = uvcpp_json::array();
            for (const clipboard_file& file : files) {
              if (file.path.empty()) continue;
              uvcpp_json item;
              item["id"] = clipboard_file_id(file);
              item["name"] = file.name;
              item["size"] = file.size;
              // True when this core just staged the file from the remote; the
              // page skips it so the two clipboard directions cannot ping-pong.
              item["echo"] = clipboard_echo_path(file.path);
              list.push_back(std::move(item));
            }
            uvcpp_json j;
            j["version"] = version;
            j["files"] = std::move(list);
            resp.set_header("Cache-Control", "no-store").json(j);
            resp.end();
          });

  app.get("/api/clipboard-file",
          [deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                 uvcpp_web_next) {
            if (!is_loopback_peer(req.peer_ip()) || !deps.clipboard) {
              resp.status(403).json_str(R"({"code":"loopback_only"})");
              resp.end();
              return;
            }
            const std::string* id = req.query("id");
            if (id == nullptr || id->empty()) {
              resp.status(400).json_str(R"({"code":"bad_id"})");
              resp.end();
              return;
            }
            // Only serve a file that is currently on the clipboard; the id is
            // a hash so the path itself never reaches the page.
            std::string path;
            {
              const auto snapshot = clipboard_snapshot();
              std::lock_guard<std::mutex> lock(snapshot->mutex);
              for (const clipboard_file& file : snapshot->files) {
                if (clipboard_file_id(file) == *id) {
                  path = file.path;
                  break;
                }
              }
            }
            if (path.empty()) {
              resp.status(404).json_str(R"({"code":"not_on_clipboard"})");
              resp.end();
              return;
            }
            // Matches the page's per-batch limit. The old 64 MiB cap made
            // copying a video from Explorer fail with a silent 413.
            constexpr uint64_t kMaxClipboardFile =
                256ull * 1024ull * 1024ull;
            // One virtual-file fetch is 64 KiB; 8 MiB leaves room for a
            // caller that reads larger slices through the same route.
            constexpr uint64_t kMaxRangeBytes = 8ull * 1024ull * 1024ull;
            std::error_code ec;
            const std::filesystem::path fs_path = path_from_utf8(path);
            if (!std::filesystem::is_regular_file(fs_path, ec) || ec) {
              resp.status(404).json_str(R"({"code":"missing_file"})");
              resp.end();
              return;
            }
            const uint64_t size = std::filesystem::file_size(fs_path, ec);
            if (ec) {
              resp.status(404).json_str(R"({"code":"missing_file"})");
              resp.end();
              return;
            }
            // `offset` / `length` turn this into a range read for the virtual
            // clipboard stream. Without them the whole file is returned, as
            // before.
            uint64_t offset = 0;
            uint64_t length = 0;
            const std::string* offset_query = req.query("offset");
            const std::string* length_query = req.query("length");
            if (offset_query != nullptr &&
                !parse_offset(*offset_query, offset)) {
              resp.status(400).json_str(R"({"code":"bad_offset"})");
              resp.end();
              return;
            }
            if (length_query != nullptr) {
              if (!parse_offset(*length_query, length) ||
                  length > kMaxRangeBytes) {
                resp.status(400).json_str(R"({"code":"bad_length"})");
                resp.end();
                return;
              }
            }
            if (offset > size) {
              resp.status(416).json_str(
                  R"({"code":"offset_out_of_range"})");
              resp.end();
              return;
            }
            const uint64_t available = size - offset;
            const uint64_t take =
                // Parenthesised: the Windows headers still define a min macro.
                length_query != nullptr ? (std::min)(length, available)
                                        : available;
            // The whole-file form keeps the old 256 MiB ceiling. A bounded
            // range is already capped above, so it needs no second limit.
            if (length_query == nullptr && take > kMaxClipboardFile) {
              resp.status(413).json_str(R"({"code":"file_too_large"})");
              resp.end();
              return;
            }
            std::ifstream in(fs_path, std::ios::binary);
            if (!in) {
              resp.status(404).json_str(R"({"code":"missing_file"})");
              resp.end();
              return;
            }
            std::string data;
            if (take != 0) {
              in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
              if (!in) {
                resp.status(416).json_str(
                    R"({"code":"offset_out_of_range"})");
                resp.end();
                return;
              }
              data.resize(static_cast<std::size_t>(take));
              in.read(data.data(), static_cast<std::streamsize>(take));
              const std::streamsize got = in.gcount();
              data.resize(got > 0 ? static_cast<std::size_t>(got) : 0);
            }
            resp.set_header("Cache-Control", "no-store")
                .body(data, "application/octet-stream")
                .end();
          });

  // ---- Remote clipboard files -> this machine's clipboard ---------------
  // Three loopback-only steps: declare a file, append its bytes in order, then
  // apply the completed set. A remote web page cannot use these routes; its
  // requests come from a non-loopback peer and are refused below.
  app.post("/api/clipboard-stage",
           [staging](uvcpp_web_request& req, uvcpp_web_response& resp,
                     uvcpp_web_next) {
             if (!is_loopback_peer(req.peer_ip())) {
               resp.status(403).json_str(R"({"code":"loopback_only"})");
               resp.end();
               return;
             }
             uvcpp_json body;
             try {
               body = uvcpp_json::parse(req.body_str());
             } catch (...) {
               resp.status(400).json_str(
                   R"({"code":"bad_stage_request"})");
               resp.end();
               return;
             }
             if (!body.is_object() || !body.contains("name") ||
                 !body["name"].is_string() || !body.contains("size") ||
                 !body["size"].is_number_integer()) {
               resp.status(400).json_str(
                   R"({"code":"bad_stage_request"})");
               resp.end();
               return;
             }
             uint64_t size = 0;
             try {
               size = body["size"].get<uint64_t>();
             } catch (...) {
               resp.status(400).json_str(
                   R"({"code":"bad_stage_request"})");
               resp.end();
               return;
             }
             std::string batch;
             if (body.contains("batch") && body["batch"].is_string()) {
               batch = body["batch"].get<std::string>();
             }
             std::string id;
             std::string path;
             const stage_error error =
                 staging->begin(body["name"].get<std::string>(), size, id,
                                path, batch);
             if (error != stage_error::none) {
               send_stage_error(resp, error);
               return;
             }
             uvcpp_json out;
             out["id"] = id;
             out["path"] = path;
             resp.set_header("Cache-Control", "no-store").json(out);
             resp.end();
           });

  app.post("/api/clipboard-stage/:id",
           [staging](uvcpp_web_request& req, uvcpp_web_response& resp,
                     uvcpp_web_next) {
             if (!is_loopback_peer(req.peer_ip())) {
               resp.status(403).json_str(R"({"code":"loopback_only"})");
               resp.end();
               return;
             }
             const std::string* id = req.param("id");
             const std::string* offset_text = req.query("offset");
             uint64_t offset = 0;
             if (id == nullptr || id->empty() || offset_text == nullptr ||
                 !parse_offset(*offset_text, offset)) {
               resp.status(400).json_str(
                   R"({"code":"bad_stage_offset"})");
               resp.end();
               return;
             }
             uint64_t received = 0;
             const stage_error error =
                 staging->append(*id, offset, req.body_data(),
                                 req.body_size(), received);
             if (error != stage_error::none) {
               send_stage_error(resp, error);
               return;
             }
             uvcpp_json out;
             out["ok"] = true;
             out["received"] = received;
             resp.set_header("Cache-Control", "no-store").json(out);
             resp.end();
           });

  // The viewer cancelled a download (clipboard changed, user pressed cancel):
  // delete its temp file now instead of waiting for the 30-minute sweep.
  app.del("/api/clipboard-stage/:id",
          [staging](uvcpp_web_request& req, uvcpp_web_response& resp,
                    uvcpp_web_next) {
            if (!is_loopback_peer(req.peer_ip())) {
              resp.status(403).json_str(R"({"code":"loopback_only"})");
              resp.end();
              return;
            }
            const std::string* id = req.param("id");
            if (id == nullptr || id->empty()) {
              resp.status(400).json_str(R"({"code":"bad_stage_request"})");
              resp.end();
              return;
            }
            staging->abort(*id);
            uvcpp_json out;
            out["ok"] = true;
            resp.set_header("Cache-Control", "no-store").json(out);
            resp.end();
          });

  app.post("/api/clipboard-apply",
           [staging, clipboard = deps.clipboard](
               uvcpp_web_request& req, uvcpp_web_response& resp,
               uvcpp_web_next) {
             if (!is_loopback_peer(req.peer_ip())) {
               resp.status(403).json_str(R"({"code":"loopback_only"})");
               resp.end();
               return;
             }
             uvcpp_json body;
             try {
               body = uvcpp_json::parse(req.body_str());
             } catch (...) {
               resp.status(400).json_str(
                   R"({"code":"bad_stage_request"})");
               resp.end();
               return;
             }
             if (!body.is_object() || !body.contains("ids") ||
                 !body["ids"].is_array()) {
               resp.status(400).json_str(
                   R"({"code":"bad_stage_request"})");
               resp.end();
               return;
             }
             std::vector<std::string> ids;
             for (const auto& item : body["ids"]) {
               if (!item.is_string()) {
                 resp.status(400).json_str(
                     R"({"code":"bad_stage_request"})");
                 resp.end();
                 return;
               }
               ids.push_back(item.get<std::string>());
             }
             if (ids.empty() || ids.size() > 256) {
               resp.status(400).json_str(
                   R"({"code":"bad_stage_request"})");
               resp.end();
               return;
             }
             std::size_t count = 0;
             const stage_error error = staging->apply(ids, count);
             if (error == stage_error::apply_failed &&
                 (!clipboard || !clipboard->available())) {
               resp.status(503).json_str(
                   R"({"code":"clipboard_unavailable"})");
               resp.end();
               return;
             }
             if (error != stage_error::none) {
               send_stage_error(resp, error);
               return;
             }
             uvcpp_json out;
             out["ok"] = true;
             out["count"] = count;
             resp.set_header("Cache-Control", "no-store").json(out);
             resp.end();
           });

  // ---- Virtual clipboard (FUSE) ---------------------------------------
  // The page forwards a `clipboard-sync.files` announcement here. The core
  // mounts the files read-only and puts the mount paths on the OS clipboard;
  // no file bytes move until the local user pastes.
  app.post("/api/clipboard-virtual-offer",
           [virtual_clipboard_service](uvcpp_web_request& req,
                                       uvcpp_web_response& resp,
                                       uvcpp_web_next) {
             if (!is_loopback_peer(req.peer_ip())) {
               resp.status(403).json_str(R"({"code":"loopback_only"})");
               resp.end();
               return;
             }
             if (!virtual_clipboard_service ||
                 !virtual_clipboard_service->available()) {
               resp.status(503).json_str(
                   R"({"code":"clipboard_virtual_unavailable"})");
               resp.end();
               return;
             }
             uvcpp_json body;
             try {
               body = uvcpp_json::parse(req.body_str());
             } catch (...) {
               resp.status(400).json_str(
                   R"({"code":"bad_virtual_offer"})");
               resp.end();
               return;
             }
             if (!body.is_object() || !body.contains("files") ||
                 !body["files"].is_array()) {
               resp.status(400).json_str(
                   R"({"code":"bad_virtual_offer"})");
               resp.end();
               return;
             }
             std::vector<virtual_offer_request_file> files;
             for (const auto& item : body["files"]) {
               if (!item.is_object() || !item.contains("id") ||
                   !item["id"].is_number_integer() ||
                   !item.contains("name") || !item["name"].is_string() ||
                   !item.contains("size") ||
                   !item["size"].is_number_integer()) {
                 resp.status(400).json_str(
                     R"({"code":"bad_virtual_offer"})");
                 resp.end();
                 return;
               }
               virtual_offer_request_file file;
               try {
                 const int64_t id = item["id"].get<int64_t>();
                 const int64_t size = item["size"].get<int64_t>();
                 if (id <= 0 || id > 0x7fffffff || size < 0) {
                   resp.status(400).json_str(
                       R"({"code":"bad_virtual_offer"})");
                   resp.end();
                   return;
                 }
                 file.id = static_cast<uint32_t>(id);
                 file.size = static_cast<uint64_t>(size);
               } catch (...) {
                 resp.status(400).json_str(
                     R"({"code":"bad_virtual_offer"})");
                 resp.end();
                 return;
               }
               file.name = item["name"].get<std::string>();
               files.push_back(std::move(file));
               if (files.size() > 64) break;
             }
             std::string session;
             if (body.contains("session") && body["session"].is_string()) {
               session = body["session"].get<std::string>();
             }
             std::size_t omitted = 0;
             std::string mount;
             std::vector<std::string> paths;
             std::string error;
             const virtual_offer_error result =
                 virtual_clipboard_service->offer(session, files, omitted,
                                                  mount, paths, error);
             if (result != virtual_offer_error::none) {
               uvcpp_json out;
               out["ok"] = false;
               out["code"] =
                   result == virtual_offer_error::unavailable
                       ? "clipboard_virtual_unavailable"
                       : result == virtual_offer_error::clipboard_unavailable
                             ? "clipboard_unavailable"
                             : result == virtual_offer_error::mount_failed
                                   ? "clipboard_virtual_mount_failed"
                                   : "bad_virtual_offer";
               out["message"] = error;
               const int status =
                   result == virtual_offer_error::bad_file
                       ? 400
                       : result == virtual_offer_error::mount_failed
                             ? 500
                             : 503;
               resp.status(status).json(out);
               resp.end();
               return;
             }
             uvcpp_json out;
             out["ok"] = true;
             out["mount"] = mount;
             out["omitted"] = omitted;
             out["paths"] = uvcpp_json::array();
             for (const std::string& path : paths) out["paths"].push_back(path);
             resp.set_header("Cache-Control", "no-store").json(out);
             resp.end();
           });

  app.post("/api/clipboard-virtual-clear",
           [virtual_clipboard_service](uvcpp_web_request& req,
                                       uvcpp_web_response& resp,
                                       uvcpp_web_next) {
             if (!is_loopback_peer(req.peer_ip())) {
               resp.status(403).json_str(R"({"code":"loopback_only"})");
               resp.end();
               return;
             }
             bool cleared = false;
             if (virtual_clipboard_service) {
               std::string mount;
               try {
                 const uvcpp_json body = uvcpp_json::parse(req.body_str());
                 if (body.is_object() && body.contains("mount") &&
                     body["mount"].is_string()) {
                   mount = body["mount"].get<std::string>();
                 }
               } catch (...) {
                 // An empty body clears unconditionally.
               }
               if (mount.empty()) {
                 virtual_clipboard_service->clear("page");
                 cleared = true;
               } else {
                 cleared = virtual_clipboard_service->clear_if(mount);
               }
             }
             uvcpp_json out;
             out["ok"] = true;
             out["cleared"] = cleared;
             resp.set_header("Cache-Control", "no-store").json(out);
             resp.end();
           });

  // The page's relay for virtual-file reads. Text frames carry the core's
  // pull request, binary frames carry the host's 0x18 reply back.
  app.websocket(
      "/ws/clipboard-virtual",
      [virtual_clipboard_service](uvcpp::uvcpp_web_ws_request& ws) {
        uvcpp::uvcpp_ws_connection* conn = ws.connection();
        if (conn == nullptr) return;
        if (!is_loopback_peer(ws.request().peer_ip())) {
          conn->close(uvcpp::ws_close_code::POLICY_VIOLATION,
                      "loopback only");
          return;
        }
        const std::string* mount = ws.query("mount");
        uint64_t bridge_id = 0;
        if (virtual_clipboard_service == nullptr || mount == nullptr ||
            mount->empty() ||
            !virtual_clipboard_service->bridge_attach(
                *mount,
                [conn](const std::string& message) {
                  conn->send_text(message.data(), message.size());
                },
                &bridge_id)) {
          conn->close(uvcpp::ws_close_code::POLICY_VIOLATION,
                      "unknown mount");
          return;
        }
        const std::string generation = *mount;
        conn->on_binary([virtual_clipboard_service](const uint8_t* data,
                                                    size_t length) {
          virtual_clipboard_service->bridge_binary(data, length);
        });
        conn->on_close([virtual_clipboard_service, generation,
                        bridge_id](uvcpp::ws_close_code,
                                   const std::string&) {
          virtual_clipboard_service->bridge_detach(generation, bridge_id);
        });
      });

  // Voice call control for the person at this machine (the host UI polls).
  app.get("/api/call",
          [deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                 uvcpp_web_next) {
            if (!is_loopback_peer(req.peer_ip()) || !deps.call) {
              resp.status(403).json_str(R"({"code":"loopback_only"})");
              resp.end();
              return;
            }
            const call_snapshot state = deps.call->snapshot();
            uvcpp_json j;
            j["state"] = state.phase == call_phase::idle
                             ? "idle"
                             : (state.phase == call_phase::ringing ? "ringing"
                                                                   : "active");
            j["started_ms"] = state.started_ms;
            j["host_microphone"] = state.host_microphone;
            resp.set_header("Cache-Control", "no-store").json(j);
            resp.end();
          });

  app.post("/api/call",
           [deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                  uvcpp_web_next) {
             if (!is_loopback_peer(req.peer_ip()) || !deps.call) {
               resp.status(403).json_str(R"({"code":"loopback_only"})");
               resp.end();
               return;
             }
             const std::string* value = req.query("action");
             const std::string action =
                 value != nullptr ? *value : std::string();
             std::string err;
             bool ok = false;
             if (action == "accept") {
               ok = deps.call->accept(err);
             } else if (action == "decline") {
               ok = deps.call->decline();
             } else if (action == "end") {
               ok = deps.call->end();
             } else {
               resp.status(400).json_str(
                   R"({"code":"bad_call_action","message":"accept, decline or end"})");
               resp.end();
               return;
             }
             if (!ok) {
               uvcpp_json body;
               body["code"] = "call_failed";
               body["message"] = err;
               resp.status(409).json(body);
               resp.end();
               return;
             }
             resp.status(204).end();
           });

  app.post("/api/ui-state",
           [deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                  uvcpp_web_next) {
             if (!deps.lock) {
               resp.status(204).end();
               return;
             }
             const std::string* sessions = req.query("sessions");
             const std::string* client = req.query("client");
             int count = 0;
             if (sessions != nullptr) {
               try {
                 count = std::stoi(*sessions);
               } catch (...) {
                 count = 0;
               }
             }
             deps.lock->set_ui_sessions(
                 client != nullptr ? *client : std::string(),
                 count < 0 ? 0 : (count > 64 ? 64 : count),
                 !is_loopback_peer(req.peer_ip()));
             resp.status(204).end();
           });

  app.post("/api/network",
           [deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                  uvcpp_web_next) {
             if (!deps.network) {
               resp.status(503).json_str(R"({"error":"network settings unavailable"})");
               resp.end();
               return;
             }
             const std::string* tcp_value = req.query("tcp");
             const std::string* udp_value = req.query("udp");
             int tcp = 0;
             int udp = 0;
             try {
               tcp = tcp_value != nullptr ? std::stoi(*tcp_value) : -1;
               udp = udp_value != nullptr ? std::stoi(*udp_value) : -1;
             } catch (...) {
               tcp = -1;
               udp = -1;
             }
             if (tcp < 1 || tcp > 65535) {
               resp.status(400).json_str(
                   R"({"error":"tcp must be between 1 and 65535"})");
               resp.end();
               return;
             }
             if (udp < 1 || udp > 65535) {
               resp.status(400).json_str(
                   R"({"error":"udp must be between 1 and 65535"})");
               resp.end();
               return;
             }
             deps.network->tcp_port.store(tcp);
             deps.network->udp_port.store(udp);
             save_port_setting("tcp_port", tcp);
             save_port_setting("udp_port", udp);
             const int bound = effective_port(deps);
             uvcpp_json j;
             j["tcp_port"] = tcp;
             j["udp_port"] = udp;
             j["bound_port"] = bound;
             j["restart_required"] =
                 tcp != bound || udp != deps.config.udp_port;
             resp.json(j);
             resp.end();
           });

  app.post("/api/lock-timeout",
           [deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                  uvcpp_web_next) {
             if (!deps.lock) {
               resp.status(503).json_str(R"({"error":"locking unavailable"})");
               resp.end();
               return;
             }
             const std::string* seconds = req.query("seconds");
             int value = -1;
             if (seconds != nullptr) {
               try {
                 value = std::stoi(*seconds);
               } catch (...) {
                 value = -1;
               }
             }
             if (value < 0 || value > 86400) {
               resp.status(400).json_str(
                   R"({"error":"seconds must be between 0 and 86400"})");
               resp.end();
               return;
             }
             deps.lock->set_timeout_seconds(value);
             save_lock_timeout_setting(value);
             uvcpp_json j;
             j["lock_timeout"] = value;
             resp.json(j);
             resp.end();
           });

  app.get("/api/device",
          [deps, virtual_clipboard_service](uvcpp_web_request& req,
                                            uvcpp_web_response& resp,
                                            uvcpp_web_next) {
            // Loopback-only, and the local UI is exempt from the idle lock.
            const std::string peer = req.peer_ip();
            const bool loopback = is_loopback_peer(peer);
            if (!loopback || !loopback_host_header(req)) {
              uvcpp_json body;
              body["type"] = "error";
              body["code"] = "loopback_only";
              body["message"] = "device credentials are only served to loopback";
              resp.status(403).json(body);
              resp.end();
              return;
            }
            uvcpp_json j;
            j["device_id"] = deps.device_id;
            j["service"] = MOLINK_CORE_SERVICE;
            j["product"] = MOLINK_PRODUCT_NAME;
            j["version"] = MOLINK_VERSION;
            j["protocol"] = 1;
            j["pid"] = current_pid();
            j["tls"] = deps.tls_enabled;
            j["token"] = deps.auth ? deps.auth->local_session_token()
                                   : std::string();
            // The desktop shell needs this to build the UI URL when it adopts
            // a core that another process (the service or helper) started.
            // The peer and Host checks above confine it to the local machine.
            j["client_token"] = deps.client_token;
            const code_view code =
                deps.auth ? deps.auth->view() : code_view{};
            if (deps.auth) deps.auth->refresh_daily();
            j["device_name"] =
                deps.auth ? deps.auth->settings().device_name : std::string();
            j["avatar"] =
                deps.auth ? deps.auth->settings().avatar : std::string();
            j["platform"] = device_platform();
            j["addresses"] = uvcpp_json::array();
            for (const std::string& address : local_ipv4_addresses()) {
              j["addresses"].push_back(address);
            }
            j["code"]["mode"] = code.mode;
            j["code"]["value"] = code.value;
            j["code"]["remaining"] = code.remaining;
            j["code"]["uses"] = code.uses;
            j["code"]["long_term_set"] = code.long_term_set;
            j["code"]["single_enabled"] = code.single_enabled;
            j["code"]["multi_enabled"] = code.multi_enabled;
            j["host"] = deps.config.host;
            j["port"] = effective_port(deps);
            j["capabilities"]["screen_capture"] =
                deps.capture != nullptr && deps.capture->is_real();
            j["capabilities"]["input_injection"] =
                deps.input != nullptr && deps.input->available();
            j["capabilities"]["clipboard"] =
                deps.clipboard != nullptr && deps.clipboard->available();
            // The viewer can mount remote clipboard files and pull ranges on
            // paste instead of downloading the whole selection on copy.
            j["capabilities"]["clipboard_virtual"] =
                virtual_clipboard_service != nullptr &&
                virtual_clipboard_service->available();
            resp.set_header("Cache-Control", "no-store").json(j);
            resp.end();
          });

  app.get("/api/discovery",
          [deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                 uvcpp_web_next) {
            if (!is_loopback_peer(req.peer_ip())) {
              resp.status(403).json_str(R"({"code":"loopback_only"})");
              resp.end();
              return;
            }
            uvcpp_json devices = uvcpp_json::array();
            if (deps.discovery) {
              for (const discovered_peer& peer : deps.discovery->peers()) {
                uvcpp_json item;
                item["device_id"] = peer.device_id;
                item["name"] = peer.name;
                item["platform"] = peer.platform;
                item["host"] = peer.host;
                item["port"] = peer.port;
                item["last_seen_ms"] = peer.last_seen_ms;
                devices.push_back(std::move(item));
              }
            }
            const app_settings settings =
                deps.auth ? deps.auth->settings() : app_settings{};
            uvcpp_json j;
            j["enabled"] = settings.allow_remote_access &&
                           settings.discovery_enabled;
            j["devices"] = std::move(devices);
            resp.set_header("Cache-Control", "no-store").json(j);
            resp.end();
          });

  // Security settings and code management are local-only. Remote peers can
  // authenticate, but must never read or change the host's long-term code.
  auto access_decision = [deps](uvcpp_web_request& req,
                                uvcpp_web_response& resp, bool allow) {
    if (!is_loopback_peer(req.peer_ip())) {
      resp.status(403).json_str(R"({"code":"loopback_only"})");
      resp.end();
      return;
    }
    const std::string* raw = req.query("id");
    const uint32_t id = raw != nullptr
                            ? static_cast<uint32_t>(
                                  std::strtoul(raw->c_str(), nullptr, 10))
                            : 0;
    // Approving can narrow this one connection: "view_only" or "assist".
    // An empty/absent policy keeps the host-wide default.
    const std::string* requested_policy = req.query("policy");
    const std::string policy =
        requested_policy != nullptr ? *requested_policy : std::string();
    if (!policy.empty() && !valid_access_policy(policy)) {
      resp.status(400).json_str(R"({"code":"invalid_access_policy"})");
      resp.end();
      return;
    }
    const bool ok =
        deps.access_requests != nullptr &&
        (allow ? deps.access_requests->approve(id, policy)
               : deps.access_requests->deny(id));
    uvcpp_json j;
    j["ok"] = ok;
    resp.status(ok ? 200 : 404).json(j);
    resp.end();
  };

  app.post("/api/access/approve",
           [access_decision](uvcpp_web_request& req, uvcpp_web_response& resp,
                             uvcpp_web_next) {
             access_decision(req, resp, true);
           });
  app.post("/api/access/deny",
           [access_decision](uvcpp_web_request& req, uvcpp_web_response& resp,
                             uvcpp_web_next) {
             access_decision(req, resp, false);
           });

  app.get("/api/windows/sessions",
          [](uvcpp_web_request& req, uvcpp_web_response& resp,
             uvcpp_web_next) {
            if (!is_loopback_peer(req.peer_ip())) {
              resp.status(403).json_str(R"({"code":"loopback_only"})");
              resp.end();
              return;
            }
            resp.set_header("Cache-Control", "no-store")
                .json_str(windows_session_command("sessions"));
            resp.end();
          });

  auto windows_session_action = [](uvcpp_web_request& req,
                                   uvcpp_web_response& resp,
                                   const char* action) {
    if (!is_loopback_peer(req.peer_ip())) {
      resp.status(403).json_str(R"({"code":"loopback_only"})");
      resp.end();
      return;
    }
    const std::string* raw = req.query("id");
    const std::string id = raw != nullptr ? *raw : std::string();
    if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos) {
      resp.status(400).json_str(R"({"ok":false,"error":"bad_session_id"})");
      resp.end();
      return;
    }
    resp.set_header("Cache-Control", "no-store")
        .json_str(windows_session_command(std::string(action) + " " + id));
    resp.end();
  };
  app.post("/api/windows/session/switch",
           [windows_session_action](uvcpp_web_request& req,
                                    uvcpp_web_response& resp,
                                    uvcpp_web_next) {
             windows_session_action(req, resp, "switch");
           });
  app.post("/api/windows/session/kick",
           [windows_session_action](uvcpp_web_request& req,
                                    uvcpp_web_response& resp,
                                    uvcpp_web_next) {
             windows_session_action(req, resp, "kick");
           });

  app.get("/api/service/status",
          [deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                 uvcpp_web_next) {
            if (!is_loopback_peer(req.peer_ip())) {
              resp.status(403).json_str(R"({"code":"loopback_only"})");
              resp.end();
              return;
            }
            const service_status_info status = query_session_service();
            uvcpp_json j;
            j["available"] = status.available;
            j["installed"] = status.installed;
            j["running"] = status.running;
            j["desired"] =
                deps.auth ? deps.auth->settings().service_desired_running
                          : true;
            j["platform"] = status.platform;
            j["autostart"] = status.autostart;
            // Windows-only diagnostics: what the service is registered with,
            // what this build expects, and whether the core can re-register
            // without another UAC prompt. Linux has no ImagePath to compare.
            if (status.platform == "windows") {
              j["path"] = status.registered_path;
              j["expected"] = status.expected_path;
              j["stale"] = status.stale;
              j["elevated"] = status.elevated;
            }
            if (!status.error.empty()) j["error"] = status.error;
            resp.set_header("Cache-Control", "no-store").json(j);
            resp.end();
          });

  auto service_action = [deps](uvcpp_web_request& req,
                               uvcpp_web_response& resp,
                               const char* action) {
    if (!is_loopback_peer(req.peer_ip())) {
      resp.status(403).json_str(R"({"code":"loopback_only"})");
      resp.end();
      return;
    }
    std::string err;
    bool ok = false;
    const std::string what(action);
    if (what == "install") {
      ok = install_session_service(err);
    } else if (what == "uninstall") {
      ok = uninstall_session_service(err);
    } else if (what == "start") {
      ok = start_session_service(err);
    } else if (what == "stop") {
      ok = stop_session_service(err);
    } else if (what == "restart") {
      ok = restart_session_service(err);
    }
    if (what == "start" || what == "install" || what == "restart") {
      if (deps.auth) deps.auth->set_service_desired_running(true);
    } else if (what == "stop" || what == "uninstall") {
      if (deps.auth) deps.auth->set_service_desired_running(false);
    }
    uvcpp_json j;
    j["ok"] = ok;
    if (!err.empty()) j["error"] = err;
    resp.status(ok ? 200 : 500).json(j);
    resp.end();
  };
  app.post("/api/service/install",
           [service_action](uvcpp_web_request& req, uvcpp_web_response& resp,
                            uvcpp_web_next) {
             service_action(req, resp, "install");
           });
  app.post("/api/service/uninstall",
           [service_action](uvcpp_web_request& req, uvcpp_web_response& resp,
                            uvcpp_web_next) {
             service_action(req, resp, "uninstall");
           });
  app.post("/api/service/start",
           [service_action](uvcpp_web_request& req, uvcpp_web_response& resp,
                            uvcpp_web_next) {
             service_action(req, resp, "start");
           });
  app.post("/api/service/stop",
           [service_action](uvcpp_web_request& req, uvcpp_web_response& resp,
                            uvcpp_web_next) {
             service_action(req, resp, "stop");
           });
  app.post("/api/service/restart",
           [service_action](uvcpp_web_request& req, uvcpp_web_response& resp,
                            uvcpp_web_next) {
             service_action(req, resp, "restart");
           });

  // ---- AV1 encoder settings -------------------------------------------
  // Host-only: the encoder lives on the machine producing the picture. The
  // active quality preset still supplies the defaults, so "auto" keeps the
  // adaptive behaviour and the explicit values only override it.
  auto encoding_json = [deps]() {
    uvcpp_json j;
    const app_settings settings =
        deps.auth ? deps.auth->settings() : app_settings{};
    j["available"] = av1_encode_available();
    j["enabled"] = settings.av1_enabled;
    j["full_frame"] = settings.av1_full_frame;
    j["device"] = settings.av1_device;
    j["speed"] = settings.av1_speed;
    j["qp"] = settings.av1_qp;
    j["max_area_percent"] = settings.av1_max_area_percent;
    j["threads"] = settings.av1_threads;
    j["keyframe_interval_seconds"] = settings.keyframe_interval_seconds;
    j["codec_preference"] = settings.stream_codec;
    j["video_mode"] = settings.video_mode;
    j["stream_enter_percent"] = settings.stream_enter_percent;
    j["devices"] = uvcpp_json::array();
    for (const av1_encoder_device& device : av1_encoder_devices()) {
      uvcpp_json item;
      item["id"] = device.id;
      item["name"] = device.name;
      item["hardware"] = device.hardware;
      item["available"] = device.available;
      if (!device.detail.empty()) item["detail"] = device.detail;
      j["devices"].push_back(std::move(item));
    }
    // Capability query only: which encoders this host can run and the
    // resolution it is capturing. The session's picture tier comes from the
    // viewer ('session-preset'), never from this endpoint.
    j["encoders"] = uvcpp_json::array();
    for (const video_encoder_backend& backend : video_encoder_backends()) {
      uvcpp_json item;
      item["id"] = backend.id;
      item["name"] = backend.name;
      item["hardware"] = backend.hardware;
      item["available"] = backend.available;
      item["av1"] = backend.av1_stream;
      item["h264"] = backend.h264_stream;
      item["vp9"] = backend.vp9_stream;
      item["vp8"] = backend.vp8_stream;
      j["encoders"].push_back(std::move(item));
    }
    if (deps.capture) {
      const display_info display = deps.capture->current_display_info();
      j["max_width"] = display.width;
      j["max_height"] = display.height;
    }
    if (deps.capture) {
      const quality_mode mode = deps.capture->quality();
      const quality_profile profile = quality_profile_for(mode);
      j["active_quality"] = quality_mode_name(mode);
      uvcpp_json effective;
      effective["speed"] = settings.av1_speed;
      bool hardware = false;
      for (const av1_encoder_device& device : av1_encoder_devices()) {
        if (!device.hardware || !device.available) continue;
        if (settings.av1_device == "auto" ||
            settings.av1_device == device.id) {
          hardware = true;
          break;
        }
      }
      effective["hardware"] = hardware;
      int enc_mode =
          av1_enc_mode_for(settings.av1_speed, settings.av1_threads);
      if (hardware) {
#if MOLINK_HAVE_NVENC
        enc_mode = nvenc_preset_for(settings.av1_speed);
#else
        enc_mode = 0;
#endif
      }
      effective["enc_mode"] = enc_mode;
      effective["threads"] =
          hardware ? 0 : av1_threads_for(settings.av1_threads);
      effective["qp"] = settings.av1_qp >= 1 && settings.av1_qp <= 63
                            ? settings.av1_qp
                            : profile.av1_qp;
      // Full-frame AV1 must cover every changed rectangle, so the area cap
      // does not apply there.
      effective["max_area_percent"] =
          settings.av1_full_frame
              ? 100
              : (settings.av1_max_area_percent >= 1 &&
                         settings.av1_max_area_percent <= 100
                     ? settings.av1_max_area_percent
                     : profile.av1_max_area_percent);
      j["effective"] = effective;
    }
    return j;
  };
  app.get("/api/encoding",
          [encoding_json](uvcpp_web_request& req, uvcpp_web_response& resp,
                          uvcpp_web_next) {
            if (!is_loopback_peer(req.peer_ip())) {
              resp.status(403).json_str(R"({"code":"loopback_only"})");
              resp.end();
              return;
            }
            resp.set_header("Cache-Control", "no-store").json(encoding_json());
            resp.end();
          });
  app.post("/api/encoding",
           [deps, encoding_json](uvcpp_web_request& req,
                                 uvcpp_web_response& resp,
                                 uvcpp_web_next) {
             if (!is_loopback_peer(req.peer_ip())) {
               resp.status(403).json_str(R"({"code":"loopback_only"})");
               resp.end();
               return;
             }
             if (!deps.auth) {
               resp.status(503).json_str(
                   R"({"code":"settings_unavailable"})");
               resp.end();
               return;
             }
             const app_settings before = deps.auth->settings();
             auto query_bool = [&req](const char* name, bool fallback) {
               const std::string* value = req.query(name);
               if (value == nullptr) return fallback;
               return *value == "1" || *value == "true" ||
                      *value == "yes";
             };
             auto query_int = [&req](const char* name, int fallback) {
               const std::string* value = req.query(name);
               if (value == nullptr) return fallback;
               try {
                 return std::stoi(*value);
               } catch (...) {
                 return fallback;
               }
             };
             const bool enabled = query_bool("enabled", before.av1_enabled);
             const bool full_frame =
                 query_bool("full_frame", before.av1_full_frame);
             const std::string* device_value = req.query("device");
             std::string device =
                 device_value != nullptr ? *device_value : before.av1_device;
             const std::string* speed_value = req.query("speed");
             std::string speed =
                 speed_value != nullptr ? *speed_value : before.av1_speed;
             const int qp = query_int("qp", before.av1_qp);
             const int max_area =
                 query_int("max_area_percent", before.av1_max_area_percent);
             const int threads = query_int("threads", before.av1_threads);
             const int keyframe_interval = query_int(
                 "keyframe_interval_seconds",
                 before.keyframe_interval_seconds);
             const int stream_enter = query_int(
                 "stream_enter_percent", before.stream_enter_percent);

             if (speed != "auto" && speed != "fast" && speed != "balanced" &&
                 speed != "quality") {
               resp.status(400).json_str(R"({"code":"invalid_speed"})");
               resp.end();
               return;
             }
             if (qp < 0 || qp > 63 || max_area < 0 || max_area > 100 ||
                 threads < 0 || threads > 64 || keyframe_interval < 0 ||
                 keyframe_interval > 60 || stream_enter < 1 ||
                 stream_enter > 100) {
               resp.status(400).json_str(R"({"code":"invalid_av1_params"})");
               resp.end();
               return;
             }
             bool known_device = device == "auto";
             for (const av1_encoder_device& candidate :
                  av1_encoder_devices()) {
               if (candidate.id == device && candidate.available) {
                 known_device = true;
                 break;
               }
             }
             // Keep a saved CPU choice valid on a build without SVT-AV1, so
             // the preference survives a reinstall of the same core.
             if (!known_device && device == "cpu-svt") known_device = true;
             if (!known_device) {
               resp.status(400).json_str(R"({"code":"invalid_device"})");
               resp.end();
               return;
             }
             deps.auth->set_av1_encoding(enabled, device, speed, qp,
                                         max_area, threads, full_frame);
             deps.auth->set_keyframe_interval_seconds(keyframe_interval);
             deps.auth->set_stream_enter_percent(stream_enter);
             // Video path: stream / mixed / tiles.
             if (const std::string* mode = req.query("video_mode");
                 mode != nullptr && !deps.auth->set_video_mode(*mode)) {
               resp.status(400).json_str(R"({"code":"invalid_video_mode"})");
               resp.end();
               return;
             }
             // Pinned stream codec: auto keeps the hardware-first ladder.
             if (const std::string* codec = req.query("stream_codec");
                 codec != nullptr && !deps.auth->set_stream_codec(*codec)) {
               resp.status(400).json_str(R"({"code":"invalid_codec"})");
               resp.end();
               return;
             }
             if (deps.capture) {
               av1_encoding_settings applied;
               applied.device = device;
               applied.speed = speed;
               applied.qp = qp;
               applied.max_area_percent = max_area;
               applied.threads = threads;
               applied.full_frame = full_frame;
               deps.capture->set_av1_settings(applied);
               deps.capture->set_keyframe_interval_ms(
                   keyframe_interval > 0 ? keyframe_interval * 1000 : 120000);
               deps.capture->set_stream_enter_percent(stream_enter);
             }
             resp.set_header("Cache-Control", "no-store").json(encoding_json());
             resp.end();
           });

  app.get("/api/settings",
          [deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                 uvcpp_web_next) {
            if (!is_loopback_peer(req.peer_ip())) {
              resp.status(403).json_str(R"({"code":"loopback_only"})");
              resp.end();
              return;
            }
            if (!deps.auth) {
              resp.status(503).json_str(R"({"code":"settings_unavailable"})");
              resp.end();
              return;
            }
            const app_settings settings = deps.auth->settings();
            uvcpp_json j;
            j["allow_remote_access"] = settings.allow_remote_access;
            j["allow_web_client"] = settings.allow_web_client;
            j["discovery_enabled"] = settings.discovery_enabled;
            j["allow_access_requests"] = settings.allow_access_requests;
            j["access_policy"] = settings.access_policy;
            j["allow_sync_between_sessions"] =
                settings.allow_sync_between_sessions;
            j["encrypt_remote"] = settings.encrypt_remote;
            j["encrypt_lan"] = settings.encrypt_lan;
            j["file_sandbox"] = settings.file_sandbox;
            j["file_sandbox_roots"] = settings.file_sandbox_roots;
            j["device_name"] = settings.device_name;
            j["avatar"] = settings.avatar;
            j["server_enabled"] = settings.server_enabled;
            j["stun_server"] = settings.stun_server;
            j["long_term_code"] = settings.long_term_code;
            // The listener is always on 0.0.0.0; access toggles apply at once.
            j["restart_required"] = false;
            resp.set_header("Cache-Control", "no-store").json(j);
            resp.end();
          });

  app.post("/api/settings",
           [deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                  uvcpp_web_next) {
             if (!is_loopback_peer(req.peer_ip())) {
               resp.status(403).json_str(R"({"code":"loopback_only"})");
               resp.end();
               return;
             }
             if (!deps.auth) {
               resp.status(503).json_str(R"({"code":"settings_unavailable"})");
               resp.end();
               return;
             }
             const app_settings before = deps.auth->settings();
             auto flag = [&req](const char* name, bool fallback) {
               const std::string* value = req.query(name);
               if (value == nullptr) return fallback;
               return *value == "1" || *value == "true" || *value == "yes";
             };
             const bool allow_remote = flag("allow_remote", before.allow_remote_access);
             const bool allow_web = flag("allow_web", before.allow_web_client);
             const bool discovery = flag("discovery", before.discovery_enabled);
             const bool access_requests =
                 flag("access_requests", before.allow_access_requests);
             const bool sync_between_sessions = flag(
                 "sync_between_sessions",
                 before.allow_sync_between_sessions);
             const bool encrypt_remote =
                 flag("encrypt_remote", before.encrypt_remote);
             const bool encrypt_lan = flag("encrypt_lan", before.encrypt_lan);
             const bool file_sandbox =
                 flag("file_sandbox", before.file_sandbox);
             const std::string* sandbox_roots =
                 req.query("file_sandbox_roots");
             const std::string* stun = req.query("stun");
             if (stun != nullptr && !deps.auth->set_stun_server(*stun)) {
               resp.status(400).json_str(R"({"code":"invalid_stun"})");
               resp.end();
               return;
             }
             const std::string* name = req.query("name");
             deps.auth->update_access(allow_remote, allow_web, discovery,
                                      name != nullptr ? *name : std::string());
             deps.auth->set_allow_access_requests(access_requests);
             // The host-wide default for new sessions. A live session keeps
             // the mask it was created with unless the local user changes it
             // through /api/sessions/access.
             const std::string* access_policy = req.query("access_policy");
             if (access_policy != nullptr &&
                 !deps.auth->set_access_policy(*access_policy)) {
               resp.status(400).json_str(R"({"code":"invalid_access_policy"})");
               resp.end();
               return;
             }
             deps.auth->set_allow_sync_between_sessions(sync_between_sessions);
             deps.auth->set_transport_security(encrypt_remote, encrypt_lan);
             deps.auth->set_file_sandbox(
                 file_sandbox,
                 sandbox_roots != nullptr ? *sandbox_roots
                                          : before.file_sandbox_roots);
             const app_settings after = deps.auth->settings();
             if (deps.on_access_changed &&
                 after.allow_remote_access != before.allow_remote_access) {
               // The QUIC listener cannot attribute a UDP peer, so it binds
               // loopback while remote access is off and moves to the
               // configured interface when the switch flips.
               deps.on_access_changed(after.allow_remote_access);
             }
             if (deps.discovery) {
               deps.discovery->set_enabled(after.allow_remote_access &&
                                           after.discovery_enabled);
             }
             if (!after.allow_remote_access && deps.registry) {
               // Close active non-loopback WebSocket sessions immediately;
               // new TCP connections are rejected by the connection hook.
               for (const session_info& session : deps.registry->snapshot()) {
                 if (session.transport &&
                     !is_loopback_peer(session.peer_ip)) {
                   session.transport->close(transport_close::going_away,
                                            "LAN access disabled");
                 }
               }
             }
             uvcpp_json j;
             j["allow_remote_access"] = after.allow_remote_access;
             j["allow_web_client"] = after.allow_web_client;
             j["discovery_enabled"] = after.discovery_enabled;
             j["allow_access_requests"] = after.allow_access_requests;
             j["access_policy"] = after.access_policy;
             j["allow_sync_between_sessions"] =
                 after.allow_sync_between_sessions;
             j["encrypt_remote"] = after.encrypt_remote;
             j["encrypt_lan"] = after.encrypt_lan;
             j["file_sandbox"] = after.file_sandbox;
             j["file_sandbox_roots"] = after.file_sandbox_roots;
             j["stun_server"] = after.stun_server;
             j["device_name"] = after.device_name;
             j["restart_required"] = false;
             resp.set_header("Cache-Control", "no-store").json(j);
             resp.end();
           });

  // Avatar update. The image data URL is too large for a query string, so the
  // local UI posts a small JSON object.
  // UI preferences (UI mode, saved devices, recents) live in ui.json because
  // the core port changes every launch and localStorage is origin-scoped.
  app.get("/api/ui-preferences",
          [deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                 uvcpp_web_next) {
            if (!is_loopback_peer(req.peer_ip()) || !deps.ui) {
              resp.status(403).json_str(R"({"code":"loopback_only"})");
              resp.end();
              return;
            }
            resp.set_header("Cache-Control", "no-store").json(deps.ui->get());
            resp.end();
          });
  app.post("/api/ui-preferences",
           [deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                  uvcpp_web_next) {
             if (!is_loopback_peer(req.peer_ip()) || !deps.ui) {
               resp.status(403).json_str(R"({"code":"loopback_only"})");
               resp.end();
               return;
             }
             uvcpp_json body;
             try {
               body = uvcpp_json::parse(req.body_str());
             } catch (...) {
               resp.status(400).json_str(R"({"code":"invalid_ui_state"})");
               resp.end();
               return;
             }
             std::string err;
             if (!deps.ui->save(body, err)) {
               uvcpp_json out;
               out["code"] = "invalid_ui_state";
               out["message"] = err;
               resp.status(400).json(out);
               resp.end();
               return;
             }
             resp.set_header("Cache-Control", "no-store").json(deps.ui->get());
             resp.end();
           });

  app.post("/api/avatar",
           [deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                  uvcpp_web_next) {
             if (!is_loopback_peer(req.peer_ip()) || !deps.auth) {
               resp.status(403).json_str(R"({"code":"loopback_only"})");
               resp.end();
               return;
             }
             uvcpp_json body;
             try {
               body = uvcpp_json::parse(req.body_str());
             } catch (...) {
               resp.status(400).json_str(R"({"code":"invalid_avatar"})");
               resp.end();
               return;
             }
             if (!body.is_object() || !body.contains("avatar") ||
                 !body["avatar"].is_string()) {
               resp.status(400).json_str(R"({"code":"invalid_avatar"})");
               resp.end();
               return;
             }
             std::string error;
             if (!deps.auth->set_avatar(body["avatar"].get<std::string>(),
                                        error)) {
               uvcpp_json out;
               out["code"] = "invalid_avatar";
               out["message"] = error;
               resp.status(400).json(out);
               resp.end();
               return;
             }
             uvcpp_json out;
             out["avatar"] = deps.auth->settings().avatar;
             resp.set_header("Cache-Control", "no-store").json(out);
             resp.end();
           });

  // Domain ("channel") code management. The code itself is issued by the
  // directory; the local UI only asks it to join or rotate.
  app.post("/api/domain",
           [deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                  uvcpp_web_next) {
             if (!is_loopback_peer(req.peer_ip()) || !deps.server) {
               resp.status(403).json_str(R"({"code":"loopback_only"})");
               resp.end();
               return;
             }
             uvcpp_json body;
             try {
               body = uvcpp_json::parse(req.body_str());
             } catch (...) {
               resp.status(400).json_str(R"({"code":"invalid_domain"})");
               resp.end();
               return;
             }
             if (!body.is_object() || !body.contains("action") ||
                 !body["action"].is_string()) {
               resp.status(400).json_str(R"({"code":"invalid_domain"})");
               resp.end();
               return;
             }
             const std::string action = body["action"].get<std::string>();
             const std::string code =
                 body.contains("code") && body["code"].is_string()
                     ? body["code"].get<std::string>()
                     : std::string();
             std::string error;
             if (!deps.server->set_domain(action, code, error)) {
               uvcpp_json out;
               out["code"] = error == "no such domain code"
                                 ? "unknown_domain"
                                 : "domain_failed";
               out["message"] = error;
               resp.status(error == "no such domain code" ? 404 : 400)
                   .json(out);
               resp.end();
               return;
             }
             const server_status status = deps.server->status();
             uvcpp_json out;
             out["domain"] = status.domain;
             out["domain_valid"] = status.domain_valid;
             resp.set_header("Cache-Control", "no-store").json(out);
             resp.end();
           });

  app.get("/api/domain/devices",
          [deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                 uvcpp_web_next) {
            if (!is_loopback_peer(req.peer_ip()) || !deps.server) {
              resp.status(403).json_str(R"({"code":"loopback_only"})");
              resp.end();
              return;
            }
            std::vector<domain_peer> peers;
            std::string error;
            if (!deps.server->list_domain_devices(peers, error)) {
              uvcpp_json out;
              out["code"] = error == "no such domain code"
                                ? "unknown_domain"
                                : "domain_failed";
              out["message"] = error;
              resp.status(error == "no such domain code" ? 404 : 400)
                  .json(out);
              resp.end();
              return;
            }
            const server_status status = deps.server->status();
            uvcpp_json out;
            out["domain"] = status.domain;
            out["domain_valid"] = status.domain_valid;
            out["devices"] = uvcpp_json::array();
            for (const domain_peer& peer : peers) {
              uvcpp_json item;
              item["id"] = peer.id;
              item["display_id"] = peer.display_id;
              item["name"] = peer.name;
              item["platform"] = peer.platform;
              item["avatar"] = peer.avatar;
              item["host"] = peer.host;
              item["port"] = peer.port;
              item["online"] = peer.online;
              item["last_seen_ms"] = peer.last_seen_ms;
              out["devices"].push_back(std::move(item));
            }
            resp.set_header("Cache-Control", "no-store").json(out);
            resp.end();
          });

  app.post("/api/auth/code",
           [deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                  uvcpp_web_next) {
             if (!is_loopback_peer(req.peer_ip()) || !deps.auth) {
               resp.status(403).json_str(R"({"code":"loopback_only"})");
               resp.end();
               return;
             }
             const std::string* mode = req.query("mode");
             const std::string* value = req.query("value");
             int uses = 20;
             if (const std::string* uses_text = req.query("uses")) {
               try {
                 uses = std::stoi(*uses_text);
               } catch (...) {
                 uses = 20;
               }
             }
             std::string error;
             const code_view code = deps.auth->set_temp(
                 mode != nullptr ? *mode : std::string("multi"), uses,
                 value != nullptr ? *value : std::string(), error);
             if (!error.empty()) {
               uvcpp_json body;
               body["code"] = "invalid_code";
               body["message"] = error;
               resp.status(400).json(body);
               resp.end();
               return;
             }
             uvcpp_json j;
             j["mode"] = code.mode;
             j["value"] = code.value;
             j["remaining"] = code.remaining;
             j["uses"] = code.uses;
             j["long_term_set"] = code.long_term_set;
             resp.set_header("Cache-Control", "no-store").json(j);
             resp.end();
           });

  app.post("/api/auth/code-policy",
           [deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                  uvcpp_web_next) {
             if (!is_loopback_peer(req.peer_ip()) || !deps.auth) {
               resp.status(403).json_str(R"({"code":"loopback_only"})");
               resp.end();
               return;
             }
             const std::string* single = req.query("single");
             const std::string* multi = req.query("multi");
             const bool single_on = single == nullptr || *single != "0";
             const bool multi_on = multi == nullptr || *multi != "0";
             std::string error;
             const code_view code =
                 deps.auth->set_code_policy(single_on, multi_on, error);
             if (!error.empty()) {
               uvcpp_json body;
               body["code"] = "invalid_code";
               body["message"] = error;
               resp.status(400).json(body);
               resp.end();
               return;
             }
             uvcpp_json j;
             j["mode"] = code.mode;
             j["value"] = code.value;
             j["remaining"] = code.remaining;
             j["uses"] = code.uses;
             j["long_term_set"] = code.long_term_set;
             j["single_enabled"] = code.single_enabled;
             j["multi_enabled"] = code.multi_enabled;
             resp.set_header("Cache-Control", "no-store").json(j);
             resp.end();
           });

  app.post("/api/auth/long-term",
           [deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                  uvcpp_web_next) {
             if (!is_loopback_peer(req.peer_ip()) || !deps.auth) {
               resp.status(403).json_str(R"({"code":"loopback_only"})");
               resp.end();
               return;
             }
             const std::string* value = req.query("value");
             std::string error;
             const std::string updated = deps.auth->set_long_term(
                 value != nullptr ? *value : std::string(), error);
             if (updated.empty()) {
               uvcpp_json body;
               body["code"] = "invalid_code";
               body["message"] = error;
               resp.status(400).json(body);
               resp.end();
               return;
             }
             uvcpp_json j;
             j["long_term_code"] = updated;
             resp.set_header("Cache-Control", "no-store").json(j);
             resp.end();
           });

  // Two-factor authentication (TOTP, RFC 6238). These routes are local-only:
  // the URI carries the shared secret, and only the person at the machine may
  // enroll or remove an authenticator.
  auto totp_body_field = [](uvcpp_web_request& req, const char* key,
                            const char* query_name) -> std::string {
    std::string value;
    if (req.body_size() > 0) {
      try {
        const uvcpp_json body = uvcpp_json::parse(req.body_str());
        if (body.is_object()) {
          const auto it = body.find(key);
          if (it != body.end() && it->is_string()) {
            value = it->get<std::string>();
          }
        }
      } catch (...) {
        // Fall through to the query string.
      }
    }
    if (value.empty()) {
      const std::string* query = req.query(query_name);
      if (query != nullptr) value = *query;
    }
    return value;
  };

  app.get("/api/2fa",
          [deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                 uvcpp_web_next) {
            if (!is_loopback_peer(req.peer_ip()) || !deps.auth) {
              resp.status(403).json_str(R"({"code":"loopback_only"})");
              resp.end();
              return;
            }
            const totp_view view = deps.auth->totp_status();
            uvcpp_json j;
            j["enabled"] = view.enabled;
            j["uri"] = view.uri;
            j["issuer"] = view.issuer;
            j["account"] = view.account;
            resp.set_header("Cache-Control", "no-store").json(j);
            resp.end();
          });

  app.post("/api/2fa/enable",
           [deps, totp_body_field](uvcpp_web_request& req,
                                   uvcpp_web_response& resp,
                                   uvcpp_web_next) {
             if (!is_loopback_peer(req.peer_ip()) || !deps.auth) {
               resp.status(403).json_str(R"({"code":"loopback_only"})");
               resp.end();
               return;
             }
             const std::string code =
                 totp_body_field(req, "code", "code");
             std::string error;
             if (!deps.auth->enable_totp(code, error)) {
               uvcpp_json body;
               body["code"] = "invalid_totp";
               body["message"] = error;
               resp.status(400).json(body);
               resp.end();
               return;
             }
             uvcpp_json j;
             j["ok"] = true;
             j["enabled"] = true;
             resp.set_header("Cache-Control", "no-store").json(j);
             resp.end();
           });

  app.post("/api/2fa/disable",
           [deps, totp_body_field](uvcpp_web_request& req,
                                   uvcpp_web_response& resp,
                                   uvcpp_web_next) {
             if (!is_loopback_peer(req.peer_ip()) || !deps.auth) {
               resp.status(403).json_str(R"({"code":"loopback_only"})");
               resp.end();
               return;
             }
             const std::string credential =
                 totp_body_field(req, "credential", "credential");
             std::string error;
             if (!deps.auth->disable_totp(credential, error)) {
               uvcpp_json body;
               body["code"] = "invalid_credential";
               body["message"] = error;
               resp.status(400).json(body);
               resp.end();
               return;
             }
             uvcpp_json j;
             j["ok"] = true;
             j["enabled"] = false;
             resp.set_header("Cache-Control", "no-store").json(j);
             resp.end();
           });

  app.get("/api/info",
          [deps, virtual_clipboard_service](uvcpp_web_request&,
                                            uvcpp_web_response& resp,
                                            uvcpp_web_next) {
            uvcpp_json j;
            j["service"] = MOLINK_CORE_SERVICE;
            j["product"] = MOLINK_PRODUCT_NAME;
            j["device_id"] = deps.device_id;
            j["version"] = MOLINK_VERSION;
            j["protocol"] = 1;
            j["pid"] = current_pid();
            j["uptime_ms"] = uptime_ms(deps.started_ms);
            j["bind"]["host"] = deps.config.host;
            j["bind"]["port"] = effective_port(deps);
            j["capabilities"]["screen_capture"] =
                deps.capture != nullptr && deps.capture->is_real();
            j["capabilities"]["input_injection"] =
                deps.input != nullptr && deps.input->available();
            j["capabilities"]["synthetic_frames"] =
                deps.capture != nullptr && !deps.capture->is_real();
            j["capabilities"]["websocket"] = true;
            j["capabilities"]["quic"] =
                deps.quic_available && deps.quic_available->load();
            j["capabilities"]["http3"] =
                deps.http3_available && deps.http3_available->load();
            if (deps.input) {
              j["input"]["backend"] = deps.input->backend();
              j["input"]["available"] = deps.input->available();
            }
            j["capabilities"]["clipboard"] =
                deps.clipboard != nullptr && deps.clipboard->available();
            j["capabilities"]["clipboard_virtual"] =
                virtual_clipboard_service != nullptr &&
                virtual_clipboard_service->available();
            if (deps.clipboard) {
              j["clipboard"]["backend"] = deps.clipboard->backend();
              j["clipboard"]["available"] = deps.clipboard->available();
            }
            if (deps.capture) {
              j["capture"]["backend"] = deps.capture->backend();
              j["capture"]["running"] = deps.capture->running();
              j["capture"]["tile_size"] = deps.tile_size;
              j["capture"]["change_threshold"] = deps.change_threshold;
              const auto latest = deps.capture->latest();
              if (latest) {
                j["capture"]["screen_w"] = latest->width;
                j["capture"]["screen_h"] = latest->height;
              }
              const capture_stats stats = deps.capture->stats();
              j["capture"]["frames"] = stats.frames;
              j["capture"]["keyframes"] = stats.keyframes;
              j["capture"]["full_grabs"] = stats.full_grabs;
              j["capture"]["rect_grabs"] = stats.rect_grabs;
              j["capture"]["bytes"] = stats.bytes;
              j["capture"]["last_dirty_w"] = stats.last_dirty_w;
              j["capture"]["last_dirty_h"] = stats.last_dirty_h;
            }
            j["sessions"] = deps.registry->size();
            resp.set_header("Cache-Control", "no-store").json(j);
            resp.end();
          });

  app.get("/api/sessions",
          [deps](uvcpp_web_request&, uvcpp_web_response& resp,
                 uvcpp_web_next) {
            uvcpp_json list = uvcpp_json::array();
            const uint64_t now = now_ms();
            for (const session_info& s : deps.registry->snapshot()) {
              uvcpp_json item;
              item["id"] = s.id;
              item["session"] = s.public_id;
              item["peer_ip"] = s.peer_ip;
              item["peer_port"] = s.peer_port;
              item["authenticated"] = s.authenticated;
              item["connected_ms"] = s.connected_ms;
              item["idle_ms"] = now > s.last_seen_ms ? now - s.last_seen_ms : 0;
              item["frames_sent"] = s.frames_sent;
              item["has_image"] = s.has_image;
              item["last_capture_seq"] = s.last_capture_seq;
              item["out_seq"] = s.out_seq;
              item["capabilities_mask"] = s.capabilities;
              uvcpp_json caps;
              const capability all[] = {
                  capability::screen,         capability::input,
                  capability::clipboard_read, capability::clipboard_write,
                  capability::files_read,     capability::files_write,
                  capability::audio,          capability::chat,
                  capability::logs,           capability::restart};
              for (capability value : all) {
                caps[capability_name(value)] =
                    has_capability(s.capabilities, value);
              }
              item["capabilities"] = std::move(caps);
              list.push_back(std::move(item));
            }
            uvcpp_json j;
            j["sessions"] = std::move(list);
            j["count"] = deps.registry->size();
            resp.set_header("Cache-Control", "no-store").json(j);
            resp.end();
          });

  // Per-session capability override (in memory). This is local-only: the
  // host user narrows or widens one live connection without changing the
  // default for future ones.
  app.post("/api/sessions/access",
           [deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                  uvcpp_web_next) {
             if (!is_loopback_peer(req.peer_ip())) {
               resp.status(403).json_str(R"({"code":"loopback_only"})");
               resp.end();
               return;
             }
             const std::string* id_text = req.query("id");
             const std::string* policy = req.query("policy");
             const uint32_t id =
                 id_text != nullptr
                     ? static_cast<uint32_t>(
                           std::strtoul(id_text->c_str(), nullptr, 10))
                     : 0;
             if (policy == nullptr || !valid_access_policy(*policy)) {
               resp.status(400).json_str(
                   R"({"code":"invalid_access_policy"})");
               resp.end();
               return;
             }
             const uint32_t mask = capabilities_for_policy(*policy);
             if (!deps.registry || !deps.registry->set_capabilities(id, mask)) {
               resp.status(404).json_str(R"({"code":"no_such_session"})");
               resp.end();
               return;
             }
             uvcpp_json j;
             j["ok"] = true;
             j["id"] = id;
             j["policy"] = *policy;
             j["capabilities_mask"] = mask;
             resp.set_header("Cache-Control", "no-store").json(j);
             resp.end();
           });

  if (!deps.serve_web) {
    app.get("/", [](uvcpp_web_request&, uvcpp_web_response& resp,
                    uvcpp_web_next) {
      uvcpp_json j;
      j["service"] = MOLINK_CORE_SERVICE;
      j["version"] = MOLINK_VERSION;
      j["health"] = "/healthz";
      j["session_endpoint"] = "/ws/session";
      resp.json(j);
      resp.end();
    });
  }
}

}  // namespace molink
