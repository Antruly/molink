#include "device_api.h"

#include <nlohmann/json.hpp>

#include <cstdlib>

#include <memory>

#include "http_common.h"
#include "logging.h"
#include "runtime.h"
#include "util.h"
#include "version.h"

namespace molink {
namespace {

using nlohmann::json;

std::string display_id(const std::string& id) {
  if (id.size() != 9) return id;
  return id.substr(0, 3) + "-" + id.substr(3, 3) + "-" + id.substr(6, 3);
}

std::string normalize_device_id(const std::string& raw) {
  std::string out;
  out.reserve(raw.size());
  for (unsigned char c : raw) {
    if (c == '-' || c == '_' || c == ' ' || c == '\t') continue;
    if (c >= 'a' && c <= 'z') c = static_cast<unsigned char>(c - 'a' + 'A');
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

std::string sanitize_name(std::string value) {
  value = trim(std::move(value));
  std::string out;
  out.reserve(value.size());
  for (unsigned char c : value) {
    if (c < 0x20 || c == 0x7f) continue;
    out.push_back(static_cast<char>(c));
  }
  if (out.size() > 96) out.resize(96);
  return out.empty() ? std::string("device") : out;
}

std::string query(const uvcpp::uvcpp_web_request& request, const char* name) {
  const std::string* value = request.query(name);
  return value == nullptr ? std::string() : trim(*value);
}

bool parse_port(const std::string& text, int& out) {
  if (text.empty()) return false;
  char* end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || value <= 0 || value > 65535) {
    return false;
  }
  out = static_cast<int>(value);
  return true;
}

bool online(const device_record& record, int offline_after_seconds,
            std::uint64_t now_ms) {
  if (record.last_seen_ms == 0) return false;
  return now_ms <=
         record.last_seen_ms +
             static_cast<std::uint64_t>(offline_after_seconds) * 1000u;
}

void rate_limited(uvcpp::uvcpp_web_response& response, int retry_after) {
  response.set_header("Retry-After", std::to_string(retry_after));
  json body;
  body["type"] = "error";
  body["code"] = "rate_limited";
  body["message"] = "too many requests";
  body["retry_after"] = retry_after;
  send_json(response, 429, body);
}

}  // namespace

void register_device_routes(uvcpp::uvcpp_web_app& app, server_runtime& runtime) {
  app.use([&runtime](uvcpp::uvcpp_web_request& request,
                     uvcpp::uvcpp_web_response& response,
                     uvcpp::uvcpp_web_next next) {
    if (runtime.stopping().load()) {
      response.status(503).json_str(R"({"type":"error","code":"shutting_down"})");
      response.end();
      return;
    }
    // The directory is consumed by the native clients, not a browser app; the
    // desktop UI calls /v1/devices directly from a loopback origin.
    response.set_header("Access-Control-Allow-Origin", "*");
    response.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    response.set_header("Access-Control-Allow-Headers", "Content-Type");
    response.set_header("X-Content-Type-Options", "nosniff");
    if (request.method() == uvcpp::http_method::HTTP_OPTIONS) {
      response.status(204).end();
      return;
    }

    const std::string path = request.path();
    const bool device_api = path.compare(0, 4, "/v1/") == 0;
    if (!device_api) {
      next();
      return;
    }

    const security_settings security = runtime.settings().security();
    ip_addr address;
    if (!effective_ip(request, security.trusted_proxies, address)) {
      send_error(response, 400, "bad_client_ip", "could not resolve the client address");
      return;
    }
    const std::string ip = canonical_ip(address);

    ban_record ban;
    if (runtime.bans().blocked(address, epoch_ms(), ban)) {
      int retry = 0;
      if (ban.expires_ms > epoch_ms()) {
        retry = static_cast<int>((ban.expires_ms - epoch_ms()) / 1000);
      }
      response.set_header("Retry-After", std::to_string(retry));
      json body;
      body["type"] = "error";
      body["code"] = "ip_banned";
      body["message"] = "this address is temporarily blocked";
      send_json(response, 403, body);
      return;
    }
    if (!policy_allows(security.policy, address)) {
      send_error(response, 403, "ip_denied",
                 "this address is not allowed by the server policy");
      return;
    }

    const limit_settings& limits = security.limits;
    int retry = 0;
    const std::string key = "dev:ip:" + ip;
    if (!runtime.limiter().allow(
            key, static_cast<double>(limits.device_ip_per_minute) / 60.0,
            limits.device_ip_burst, epoch_ms(), &retry)) {
      rate_limited(response, retry);
      return;
    }
    next();
  });

  app.get("/", [&runtime](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& response,
                          uvcpp::uvcpp_web_next) {
    const server_settings settings = runtime.settings().server();
    json body;
    body["service"] = MOLINK_SERVER_PRODUCT;
    body["version"] = MOLINK_SERVER_VERSION;
    body["public_url"] = settings.public_url;
    body["endpoints"]["register"] = "POST /v1/devices/register";
    body["endpoints"]["heartbeat"] = "POST /v1/devices/heartbeat";
    body["endpoints"]["lookup"] = "GET /v1/devices?id=ABC-DEF-GHJ";
    body["endpoints"]["unregister"] = "POST /v1/devices/unregister";
    send_json(response, 200, body);
  });

  auto health = [&runtime](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& response,
                           uvcpp::uvcpp_web_next next) {
    auto result = std::make_shared<json>();
    runtime.executor().read(
        runtime.device_app().loop(),
        [](uvcpp::uvcpp_db_client& client, std::string& error) {
          const auto status = client.ping();
          if (!uvcpp::uvcpp_db_ok(status)) {
            error = client.last_error();
            return false;
          }
          return true;
        },
        [&response, next, result, &runtime](bool ok, const std::string&) {
          (*result)["status"] = ok ? "ok" : "degraded";
          (*result)["service"] = MOLINK_SERVER_PRODUCT;
          (*result)["version"] = MOLINK_SERVER_VERSION;
          (*result)["database"] = ok ? "ok" : "unavailable";
          (*result)["devices"] = 0;
          send_json(response, ok ? 200 : 503, *result);
          next();
        });
  };
  app.get("/healthz", health);
  app.get("/api/health", health);

  app.post("/v1/devices/register",
           [&runtime](uvcpp::uvcpp_web_request& request,
                      uvcpp::uvcpp_web_response& response,
                      uvcpp::uvcpp_web_next next) {
             const server_settings settings = runtime.settings().server();
             const security_settings security = runtime.settings().security();
             const limit_settings& limits = security.limits;
             ip_addr address;
             if (!effective_ip(request, security.trusted_proxies, address)) {
               send_error(response, 400, "bad_client_ip",
                          "could not resolve the client address");
               return;
             }
             const std::string ip = canonical_ip(address);

             if (!settings.registration_token.empty()) {
               const std::string invite = query(request, "invite");
               if (!constant_time_equals(invite, settings.registration_token)) {
                 send_error(response, 403, "invalid_invite",
                            "the registration invite is not valid");
                 return;
               }
             }

             int retry = 0;
             if (!runtime.limiter().allow(
                     "register:" + ip,
                     static_cast<double>(limits.register_per_hour) / 3600.0,
                     limits.register_burst, epoch_ms(), &retry)) {
               rate_limited(response, retry);
               return;
             }

             const std::string name = sanitize_name(query(request, "name"));
             int port = 0;
             if (!parse_port(query(request, "port"), port)) {
               send_error(response, 400, "bad_port", "port must be 1..65535");
               return;
             }
             std::string version = query(request, "version");
             if (version.size() > 32) version.resize(32);

             const std::string token = random_hex(16);
             if (token.empty()) {
               send_error(response, 500, "internal_error",
                          "secure random generator is unavailable");
               return;
             }
             struct state_t {
               device_record device;
               std::string code;
               std::string message;
             };
             auto state = std::make_shared<state_t>();
             state->device.name = name;
             state->device.peer_ip = ip;
             state->device.port = port;
             state->device.version = version;
             state->device.token_hash = sha256_hex(token);
             state->device.address.clear();

             runtime.executor().write(
                 runtime.device_app().loop(),
                 [&runtime, state, ip, limit = limits.devices_per_ip](
                     uvcpp::uvcpp_db_client& client, std::string& error) {
                   std::size_t count = 0;
                   if (!runtime.devices().count_for_ip(
                           client, runtime.db().dialect(), ip, count, error)) {
                     return false;
                   }
                   if (count >= static_cast<std::size_t>(limit)) {
                     state->code = "ip_quota";
                     state->message = "too many devices registered from this address";
                     return false;
                   }
                   if (!runtime.devices().insert(client, runtime.db().dialect(),
                                                 state->device, error)) {
                     return false;
                   }
                   return true;
                 },
                 [&response, next, state, token, &runtime](
                     bool ok, const std::string& error) {
                   if (!ok) {
                     if (!state->code.empty()) {
                       send_error(response, 429, state->code, state->message);
                     } else {
                       log_error("device register: %s", error.c_str());
                       send_error(response, 503, "database_unavailable",
                                  "the device directory is unavailable");
                     }
                     next();
                     return;
                   }
                   const server_settings settings =
                       runtime.settings().server();
                   json body;
                   body["device_id"] = state->device.id;
                   body["display_id"] = display_id(state->device.id);
                   body["token"] = token;
                   body["server_time"] = epoch_ms();
                   body["public_url"] = settings.public_url;
                   send_json(response, 200, body);
                   next();
                 });
           });

  app.post("/v1/devices/heartbeat",
           [&runtime](uvcpp::uvcpp_web_request& request,
                      uvcpp::uvcpp_web_response& response,
                      uvcpp::uvcpp_web_next next) {
             const security_settings security = runtime.settings().security();
             ip_addr address;
             if (!effective_ip(request, security.trusted_proxies, address)) {
               send_error(response, 400, "bad_client_ip",
                          "could not resolve the client address");
               return;
             }
             const std::string ip = canonical_ip(address);
             const std::string id = normalize_device_id(query(request, "id"));
             const std::string token = query(request, "token");
             if (id.empty() || token.empty()) {
               send_error(response, 400, "bad_request",
                          "id and token are required");
               return;
             }
             int retry = 0;
             if (!runtime.limiter().allow(
                     "hb:dev:" + id,
                     static_cast<double>(security.limits
                                             .heartbeat_per_device_per_minute) /
                         60.0,
                     security.limits.heartbeat_burst, epoch_ms(), &retry)) {
               rate_limited(response, retry);
               return;
             }
             if (!runtime.limiter().allow(
                     "hb:ip:" + ip,
                     static_cast<double>(security.limits
                                             .heartbeat_per_ip_per_minute) /
                         60.0,
                     security.limits.heartbeat_per_ip_per_minute, epoch_ms())) {
               rate_limited(response, 60);
               return;
             }
             int port = 0;
             parse_port(query(request, "port"), port);
             const std::string version = query(request, "version");
             auto found = std::make_shared<bool>(false);
             runtime.executor().write(
                 runtime.device_app().loop(),
                 [&runtime, id, token, ip, port, version, found](
                     uvcpp::uvcpp_db_client& client, std::string& error) {
                   const std::string token_hash = sha256_hex(token);
                   if (!runtime.devices().heartbeat(
                           client, runtime.db().dialect(), id, token_hash, "",
                           ip, port, version, *found, error)) {
                     return false;
                   }
                   return true;
                 },
                 [&response, next, found](bool ok, const std::string& error) {
                   if (!ok) {
                     log_error("device heartbeat: %s", error.c_str());
                     send_error(response, 503, "database_unavailable",
                                "the device directory is unavailable");
                   } else if (!*found) {
                     send_error(response, 401, "invalid_token",
                                "unknown device or token");
                   } else {
                     response.status(204).end();
                   }
                   next();
                 });
           });

  app.post("/v1/devices/unregister",
           [&runtime](uvcpp::uvcpp_web_request& request,
                      uvcpp::uvcpp_web_response& response,
                      uvcpp::uvcpp_web_next next) {
             const security_settings security = runtime.settings().security();
             ip_addr address;
             effective_ip(request, security.trusted_proxies, address);
             const std::string ip = canonical_ip(address);
             const std::string id = normalize_device_id(query(request, "id"));
             const std::string token = query(request, "token");
             if (id.empty() || token.empty()) {
               send_error(response, 400, "bad_request",
                          "id and token are required");
               return;
             }
             int retry = 0;
             if (!runtime.limiter().allow(
                     "unregister:" + ip,
                     static_cast<double>(security.limits
                                             .unregister_per_minute) /
                         60.0,
                     security.limits.unregister_burst, epoch_ms(), &retry)) {
               rate_limited(response, retry);
               return;
             }
             auto found = std::make_shared<bool>(false);
             runtime.executor().write(
                 runtime.device_app().loop(),
                 [&runtime, id, token, found](uvcpp::uvcpp_db_client& client,
                                               std::string& error) {
                   return runtime.devices().remove(
                       client, runtime.db().dialect(), id, sha256_hex(token),
                       *found);
                 },
                 [&response, next, found](bool ok, const std::string&) {
                   if (!ok) {
                     send_error(response, 503, "database_unavailable",
                                "the device directory is unavailable");
                   } else if (!*found) {
                     send_error(response, 401, "invalid_token",
                                "unknown device or token");
                   } else {
                     response.status(204).end();
                   }
                   next();
                 });
           });

  auto lookup = [&runtime](uvcpp::uvcpp_web_request& request,
                           uvcpp::uvcpp_web_response& response,
                           uvcpp::uvcpp_web_next next) {
    const security_settings security = runtime.settings().security();
    const std::string id = normalize_device_id(query(request, "id"));
    if (id.empty()) {
      send_error(response, 400, "bad_request", "id is required");
      return;
    }
    ip_addr address;
    if (!effective_ip(request, security.trusted_proxies, address)) {
      send_error(response, 400, "bad_client_ip",
                 "could not resolve the client address");
      return;
    }
    int retry = 0;
    if (!runtime.limiter().allow(
            "lookup:" + canonical_ip(address),
            static_cast<double>(security.limits.lookup_per_minute) / 60.0,
            security.limits.lookup_burst, epoch_ms(), &retry)) {
      rate_limited(response, retry);
      return;
    }
    auto record = std::make_shared<device_record>();
    auto not_found = std::make_shared<bool>(false);
    runtime.executor().read(
        runtime.device_app().loop(),
        [&runtime, id, record, not_found](uvcpp::uvcpp_db_client& client,
                                           std::string& error) {
          if (!runtime.devices().find(client, runtime.db().dialect(), id,
                                      *record, error)) {
            if (error == "device not found") {
              *not_found = true;
              error.clear();
              return true;
            }
            return false;
          }
          return true;
        },
        [&response, next, record, not_found, &runtime](bool ok,
                                                       const std::string& error) {
          if (!ok) {
            log_error("device lookup: %s", error.c_str());
            send_error(response, 503, "database_unavailable",
                       "the device directory is unavailable");
            next();
            return;
          }
          if (*not_found) {
            send_error(response, 404, "not_found", "unknown device");
            next();
            return;
          }
          const server_settings settings = runtime.settings().server();
          const std::uint64_t now = epoch_ms();
          json body;
          body["id"] = record->id;
          body["display_id"] = display_id(record->id);
          body["name"] = record->name;
          body["address"] = record->address;
          body["peer_ip"] = record->peer_ip;
          body["port"] = record->port;
          body["version"] = record->version;
          body["online"] =
              online(*record, settings.offline_after_seconds, now);
          body["created_ms"] = record->created_ms;
          body["last_seen_ms"] = record->last_seen_ms;
          body["public_url"] = settings.public_url;
          send_json(response, 200, body);
          next();
        });
  };
  app.get("/v1/devices", lookup);
  app.get("/v1/devices/lookup", lookup);
}

}  // namespace molink
