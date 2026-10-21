#include "api.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_json.h>

#include "config.h"
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

using uvcpp::uvcpp_json;
using uvcpp::uvcpp_web_next;
using uvcpp::uvcpp_web_request;
using uvcpp::uvcpp_web_response;

bool origin_allowed(const std::string& origin) {
  // Tauri v2 serves the shell from these origins, depending on platform. The
  // two localhost entries are the Vite dev server.
  return origin == "tauri://localhost" ||
         origin == "http://tauri.localhost" ||
         origin == "https://tauri.localhost" ||
         origin == "http://localhost:1420" ||
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

uint64_t uptime_ms(uint64_t started) {
  const uint64_t now = now_ms();
  return now > started ? now - started : 0;
}

}  // namespace

void register_http_routes(uvcpp::uvcpp_web_app& app, const api_deps& deps) {
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

  // ---- Token guard for /api/* -----------------------------------------
  // /healthz and /api/health stay open: the shell's readiness probe and
  // monitoring need them, and they expose nothing beyond version and uptime.
  const auto token = std::make_shared<const std::string>(deps.config.token);
  app.use([token](uvcpp_web_request& req, uvcpp_web_response& resp,
                  uvcpp_web_next next) {
    const std::string& path = req.path();
    const bool is_api = path.compare(0, 5, "/api/") == 0;
    if (!is_api || path == "/api/health") {
      next();
      return;
    }
    if (constant_time_equals(bearer_token(req), *token)) {
      next();
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

  app.get("/api/info",
          [deps](uvcpp_web_request&, uvcpp_web_response& resp,
                 uvcpp_web_next) {
            uvcpp_json j;
            j["service"] = MOLINK_CORE_SERVICE;
            j["product"] = MOLINK_PRODUCT_NAME;
            j["version"] = MOLINK_VERSION;
            j["protocol"] = 1;
            j["pid"] = current_pid();
            j["uptime_ms"] = uptime_ms(deps.started_ms);
            j["bind"]["host"] = deps.config.host;
            j["bind"]["port"] = deps.config.port;
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
