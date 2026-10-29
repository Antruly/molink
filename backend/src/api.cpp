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

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
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

#include "config.h"
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

}  // namespace

void register_http_routes(uvcpp::uvcpp_web_app& app, const api_deps& deps) {
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
    // headers. Gating the HTML entry point plus the state/device/login APIs is
    // enough; a browser cannot use the UI without the HTML and cannot obtain a
    // session token without the client token.
    // /api/transports is the capability advertisement peer cores fetch before
    // they open a data channel; gating it behind the shell token would make
    // core-to-core transport negotiation impossible. It exposes only ports
    // and availability, which /healthz already implies.
    const bool gated = path == "/" || path == "/index.html" ||
                       path == "/api/state" || path == "/api/login" ||
                       path == "/api/device";
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
                      "GET, POST, OPTIONS");
      resp.set_header("Access-Control-Allow-Headers",
                      "Authorization, Content-Type, X-MoLink-Token");
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
             if (!deps.auth ||
                 !deps.auth->authenticate(bearer_token(req),
                                          /*consume=*/true)) {
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
            std::error_code ec;
            const std::filesystem::path fs_path(path);
            if (!std::filesystem::is_regular_file(fs_path, ec) || ec) {
              resp.status(404).json_str(R"({"code":"missing_file"})");
              resp.end();
              return;
            }
            const uint64_t size = std::filesystem::file_size(fs_path, ec);
            if (ec || size > kMaxClipboardFile) {
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
            std::string data((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
            resp.set_header("Cache-Control", "no-store")
                .body(data, "application/octet-stream")
                .end();
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
          [deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                 uvcpp_web_next) {
            // Loopback-only, and the local UI is exempt from the idle lock.
            const std::string peer = req.peer_ip();
            const bool loopback = is_loopback_peer(peer);
            if (!loopback) {
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
            j["token"] = deps.auth ? deps.auth->local_session_token()
                                   : std::string();
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
    const bool ok =
        deps.access_requests != nullptr &&
        (allow ? deps.access_requests->approve(id)
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

             if (speed != "auto" && speed != "fast" && speed != "balanced" &&
                 speed != "quality") {
               resp.status(400).json_str(R"({"code":"invalid_speed"})");
               resp.end();
               return;
             }
             if (qp < 0 || qp > 63 || max_area < 0 || max_area > 100 ||
                 threads < 0 || threads > 64 || keyframe_interval < 0 ||
                 keyframe_interval > 60) {
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
            j["encrypt_remote"] = settings.encrypt_remote;
            j["encrypt_lan"] = settings.encrypt_lan;
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
             const bool encrypt_remote =
                 flag("encrypt_remote", before.encrypt_remote);
             const bool encrypt_lan = flag("encrypt_lan", before.encrypt_lan);
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
             deps.auth->set_transport_security(encrypt_remote, encrypt_lan);
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
             j["encrypt_remote"] = after.encrypt_remote;
             j["encrypt_lan"] = after.encrypt_lan;
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

  app.get("/api/info",
          [deps](uvcpp_web_request&, uvcpp_web_response& resp,
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
              list.push_back(std::move(item));
            }
            uvcpp_json j;
            j["sessions"] = std::move(list);
            j["count"] = deps.registry->size();
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
