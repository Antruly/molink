#include "api.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_json.h>

#include "config.h"
#include "discovery.h"
#include "identity.h"
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
    const bool gated = path == "/" || path == "/index.html" ||
                       path == "/api/state" || path == "/api/login" ||
                       path == "/api/device" || path == "/api/transports";
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
                              path == "/api/login" ||
                              path == "/api/transports");
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
  // While locked only the state probe and the login/unlock endpoint remain
  // reachable; everything else (including the local device credentials) is
  // blocked until the verification code is entered again.
  app.use([deps](uvcpp_web_request& req, uvcpp_web_response& resp,
                 uvcpp_web_next next) {
    if (!deps.lock) {
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
        path == "/api/transports") {
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
            j["allow_access_requests"] = current_settings.allow_access_requests;
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
            j["locked"] = deps.lock != nullptr && deps.lock->locked();
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
          [deps](uvcpp_web_request&, uvcpp_web_response& resp,
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
            add("http2", "api", deps.http2_available, false, web_port,
                "h2c prior knowledge on the web port");
            add("http3", "api", false, true, http3_port,
                "needs TLS + QUIC front-end");
            add("quic", "data", false, true, quic_port,
                "direct data channel (planned)");
            add("tcp", "data", false, false, tcp_data_port,
                "direct data channel (planned)");
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
           [deps](uvcpp_web_request&, uvcpp_web_response& resp,
                  uvcpp_web_next) {
             if (deps.lock) deps.lock->touch();
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
                 count < 0 ? 0 : (count > 64 ? 64 : count));
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
            if (deps.lock && deps.lock->locked()) {
              uvcpp_json body;
              body["type"] = "error";
              body["code"] = "locked";
              body["message"] = "the UI is locked";
              resp.status(423).json(body);
              resp.end();
              return;
            }
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
            j["device_name"] =
                deps.auth ? deps.auth->settings().device_name : std::string();
            j["code"]["mode"] = code.mode;
            j["code"]["value"] = code.value;
            j["code"]["remaining"] = code.remaining;
            j["code"]["uses"] = code.uses;
            j["code"]["long_term_set"] = code.long_term_set;
            j["host"] = deps.config.host;
            j["port"] = effective_port(deps);
            j["capabilities"]["screen_capture"] =
                deps.capture != nullptr && deps.capture->is_real();
            j["capabilities"]["input_injection"] =
                deps.input != nullptr && deps.input->available();
            j["capabilities"]["clipboard"] =
                deps.clipboard != nullptr && deps.clipboard->available();
            if (deps.lock) deps.lock->touch();
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
            j["device_name"] = settings.device_name;
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
             const std::string* name = req.query("name");
             deps.auth->update_access(allow_remote, allow_web, discovery,
                                      name != nullptr ? *name : std::string());
             deps.auth->set_allow_access_requests(access_requests);
             const app_settings after = deps.auth->settings();
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
             j["device_name"] = after.device_name;
             j["restart_required"] = false;
             resp.set_header("Cache-Control", "no-store").json(j);
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
