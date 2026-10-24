#include "admin_api.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "admin_ui.h"
#include "auth/password.h"
#include "http_common.h"
#include "logging.h"
#include "runtime.h"
#include "util.h"
#include "version.h"

namespace molink {
namespace {

using nlohmann::json;

constexpr const char* kSessionCookie = "molink_admin_session";
constexpr const char* kCsrfCookie = "molink_admin_csrf";

int role_rank(const std::string& role) {
  if (role == "admin") return 3;
  if (role == "operator") return 2;
  if (role == "viewer") return 1;
  return 0;
}

std::string display_id(const std::string& id) {
  if (id.size() != 9) return id;
  return id.substr(0, 3) + "-" + id.substr(3, 3) + "-" + id.substr(6, 3);
}

json user_json(const admin_user& user) {
  json out;
  out["id"] = user.id;
  out["username"] = user.username;
  out["display_name"] = user.display_name;
  out["role"] = user.role;
  out["disabled"] = user.disabled;
  out["must_change_password"] = user.must_change_password;
  out["created_ms"] = user.created_ms;
  out["last_login_ms"] = user.last_login_ms;
  return out;
}

json device_json(const device_record& record, int offline_after_seconds,
                 std::uint64_t now) {
  json out;
  out["id"] = record.id;
  out["display_id"] = display_id(record.id);
  out["name"] = record.name;
  out["address"] = record.address;
  out["peer_ip"] = record.peer_ip;
  out["port"] = record.port;
  out["version"] = record.version;
  out["online"] =
      record.last_seen_ms != 0 &&
      now <= record.last_seen_ms +
                 static_cast<std::uint64_t>(offline_after_seconds) * 1000u;
  out["created_ms"] = record.created_ms;
  out["last_seen_ms"] = record.last_seen_ms;
  return out;
}

void reply_error(http_reply& reply, int status, const std::string& code,
                 const std::string& message) {
  reply.status = status;
  json body;
  body["type"] = "error";
  body["code"] = code;
  body["message"] = message;
  reply.body = body.dump();
}

int auth_status(auth_code code) {
  switch (code) {
    case auth_code::invalid_credentials:
    case auth_code::unauthorized:
    case auth_code::disabled:
      return 401;
    case auth_code::setup_closed:
    case auth_code::conflict:
    case auth_code::last_admin:
    case auth_code::self_delete:
      return 409;
    case auth_code::not_found:
      return 404;
    case auth_code::weak_password:
    case auth_code::invalid_username:
    case auth_code::invalid_role:
      return 400;
    case auth_code::internal:
    default:
      return 503;
  }
}

void reply_auth_error(http_reply& reply, const auth_error& error) {
  reply_error(reply, auth_status(error.code), auth_code_name(error.code),
              error.message);
}

bool request_secure(server_runtime& runtime,
                    const uvcpp::uvcpp_web_request& request,
                    const security_settings& security) {
  const bool https = request_is_https(
      request, runtime.admin_tls(), security.session.trust_forwarded_proto);
  if (security.session.secure_cookie == "always") return true;
  if (security.session.secure_cookie == "never") return false;
  return https;
}

bool origin_ok(const uvcpp::uvcpp_web_request& request, bool secure) {
  const std::string origin = request.header("Origin", "");
  if (origin.empty()) return true;
  if (origin == "null") return false;
  const std::string host = request.header("Host", "");
  if (host.empty()) return false;
  return origin == (secure ? "https://" : "http://") + host;
}

void rate_limited(http_reply& reply, int retry_after) {
  reply.status = 429;
  reply.headers.emplace_back("Retry-After", std::to_string(retry_after));
  json body;
  body["type"] = "error";
  body["code"] = "rate_limited";
  body["message"] = "too many requests";
  body["retry_after"] = retry_after;
  reply.body = body.dump();
}

void set_auth_cookies(http_reply& reply, const login_result& result,
                      std::uint64_t now, const security_settings& security,
                      bool secure) {
  long max_age = static_cast<long>(security.session.absolute_seconds);
  if (result.expires_ms > now) {
    max_age = static_cast<long>((result.expires_ms - now) / 1000);
  }
  if (max_age < 1) max_age = 1;
  reply.set_cookies.push_back(
      make_cookie(kSessionCookie, result.session_token, static_cast<int>(max_age),
                  true, secure));
  reply.set_cookies.push_back(
      make_cookie(kCsrfCookie, result.csrf_token, static_cast<int>(max_age),
                  false, secure));
}

// --- login failure ladder -------------------------------------------------

struct failure_entry {
  std::uint64_t window_start = 0;
  int failures = 0;
  int strikes = 0;
};

std::mutex g_failure_mutex;
std::unordered_map<std::string, failure_entry> g_failures;

int record_login_failure(const std::string& ip, const security_settings& security,
                         std::uint64_t now) {
  if (is_loopback_peer(ip) && !security.login.throttle_loopback) return 0;
  std::lock_guard<std::mutex> lock(g_failure_mutex);
  failure_entry& entry = g_failures[ip];
  if (entry.window_start == 0 ||
      now > entry.window_start +
                static_cast<std::uint64_t>(security.login.failure_window_seconds) *
                    1000u) {
    entry.window_start = now;
    entry.failures = 0;
  }
  entry.failures += 1;
  if (entry.failures % security.login.failure_limit != 0) return 0;
  entry.strikes += 1;
  const std::size_t index = std::min<std::size_t>(
      static_cast<std::size_t>(entry.strikes - 1),
      security.login.ban_ladder_seconds.size() - 1);
  return security.login.ban_ladder_seconds[index];
}

void clear_login_failures(const std::string& ip) {
  std::lock_guard<std::mutex> lock(g_failure_mutex);
  g_failures.erase(ip);
}

void schedule_auto_ban(server_runtime& runtime, const std::string& ip,
                       int seconds) {
  if (seconds <= 0) return;
  const std::uint64_t now = epoch_ms();
  ban_record ban;
  ban.id = "ban_" + random_hex(12);
  ban.target = ip;
  ban.target_kind = "ip";
  ban.reason = "login_failures";
  ban.source = "auto";
  ban.created_ms = now;
  ban.expires_ms = now + static_cast<std::uint64_t>(seconds) * 1000u;
  ban.created_by = "system";
  ban.note = "temporary ban after repeated login failures";
  runtime.executor().write(
      runtime.admin_app().loop(),
      [&runtime, ban](uvcpp::uvcpp_db_client& client, std::string& error) {
        return runtime.bans_repository().add(client, runtime.db().dialect(),
                                             ban, error);
      },
      [&runtime, ban](bool ok, const std::string& error) {
        if (ok) {
          runtime.bans().add(ban);
        } else {
          log_error("auto ban: %s", error.c_str());
        }
      });
}

void append_audit(server_runtime& runtime, uvcpp::uvcpp_db_client& client,
                  const admin_user& actor, const std::string& event,
                  const std::string& target,
                  const std::string& details = "{}") {
  audit_record record;
  record.ts_ms = epoch_ms();
  record.event = event;
  record.result = "ok";
  record.actor_id = actor.id;
  record.actor_name = actor.username;
  record.role = actor.role;
  record.target = target;
  record.details_json = details;
  std::string error;
  runtime.audit_repository_ref().append(client, runtime.db().dialect(), record,
                                        error);
}

// --- authenticated route wrapper -----------------------------------------

using admin_op = std::function<void(uvcpp::uvcpp_db_client&, const admin_user&,
                                    const session_record&, http_reply&)>;

void with_admin(server_runtime& runtime,
                const uvcpp::uvcpp_web_request& request,
                uvcpp::uvcpp_web_response& response, uvcpp::uvcpp_web_next next,
                int required_rank, bool require_csrf, admin_op op) {
  const security_settings security = runtime.settings().security();
  const bool secure = request_secure(runtime, request, security);
  const std::string raw_session = cookie_value(request, kSessionCookie);
  if (raw_session.empty()) {
    send_error(response, 401, "unauthorized", "a session is required");
    return;
  }
  if (require_csrf) {
    if (!origin_ok(request, secure)) {
      send_error(response, 403, "origin_rejected", "the request origin is not allowed");
      return;
    }
    const std::string cookie = cookie_value(request, kCsrfCookie);
    const std::string header = request.header("X-CSRF-Token", "");
    if (cookie.empty() || !constant_time_equals(cookie, header)) {
      send_error(response, 403, "csrf_failed", "the CSRF token is missing or invalid");
      return;
    }
  }
  int retry = 0;
  if (!runtime.limiter().allow(
          "admin:api:" + raw_session,
          static_cast<double>(security.limits.admin_api_per_minute) / 60.0,
          security.limits.admin_api_burst, epoch_ms(), &retry)) {
    response.set_header("Retry-After", std::to_string(retry));
    json body;
    body["type"] = "error";
    body["code"] = "rate_limited";
    body["message"] = "too many requests";
    body["retry_after"] = retry;
    send_json(response, 429, body);
    return;
  }

  struct state_t {
    std::string code;
    std::string message;
    http_reply reply;
  };
  auto state = std::make_shared<state_t>();
  runtime.executor().write(
      runtime.admin_app().loop(),
      [&runtime, raw_session, required_rank, state, op](
          uvcpp::uvcpp_db_client& client, std::string& error) {
        admin_user user;
        session_record session;
        auth_error auth_error_value;
        if (!runtime.auth().validate_session(
                client, runtime.db().dialect(), raw_session, user, session,
                auth_error_value)) {
          state->code = auth_code_name(auth_error_value.code);
          state->message = auth_error_value.message;
          return false;
        }
        if (role_rank(user.role) < required_rank) {
          state->code = "forbidden";
          state->message = "this account does not have the required role";
          return false;
        }
        op(client, user, session, state->reply);
        return true;
      },
      [&response, next, state](bool ok, const std::string& error) {
        if (!state->code.empty()) {
          const int status = state->code == "forbidden" ? 403 : 401;
          send_error(response, status, state->code, state->message);
        } else if (!ok) {
          log_error("admin api: %s", error.c_str());
          send_error(response, 503, "database_unavailable",
                     "the database is unavailable");
        } else {
          send_reply(response, state->reply);
        }
        next();
      });
}

std::string body_string(const json& body, const char* key) {
  if (!body.contains(key) || !body[key].is_string()) return std::string();
  return body[key].get<std::string>();
}

bool body_bool(const json& body, const char* key, bool fallback) {
  if (!body.contains(key) || !body[key].is_boolean()) return fallback;
  return body[key].get<bool>();
}

int body_int(const json& body, const char* key, int fallback) {
  if (!body.contains(key) || !body[key].is_number_integer()) return fallback;
  return body[key].get<int>();
}

std::size_t page_number(const uvcpp::uvcpp_web_request& request,
                        const char* name, std::size_t fallback,
                        std::size_t maximum) {
  const std::string* value = request.query(name);
  if (value == nullptr) return fallback;
  try {
    const long long parsed = std::stoll(*value);
    if (parsed < 0) return fallback;
    return std::min<std::size_t>(static_cast<std::size_t>(parsed), maximum);
  } catch (...) {
    return fallback;
  }
}

}  // namespace

void register_admin_routes(uvcpp::uvcpp_web_app& app, server_runtime& runtime,
                           const std::string& ui_root) {
  app.use([&runtime](uvcpp::uvcpp_web_request& request,
                     uvcpp::uvcpp_web_response& response,
                     uvcpp::uvcpp_web_next next) {
    if (runtime.stopping().load()) {
      response.status(503).json_str(R"({"type":"error","code":"shutting_down"})");
      response.end();
      return;
    }
    apply_security_headers(response);
    if (request.method() == uvcpp::http_method::HTTP_OPTIONS) {
      response.status(204).end();
      return;
    }
    const std::string path = request.path();
    const bool api = path.compare(0, 11, "/api/admin/") == 0;
    if (!api) {
      next();
      return;
    }

    const security_settings security = runtime.settings().security();
    ip_addr address;
    if (!effective_ip(request, security.trusted_proxies, address)) {
      send_error(response, 400, "bad_client_ip",
                 "could not resolve the client address");
      return;
    }
    ban_record ban;
    if (runtime.bans().blocked(address, epoch_ms(), ban)) {
      response.set_header("Retry-After", "3600");
      send_error(response, 403, "ip_banned",
                 "this address is temporarily blocked");
      return;
    }
    if (!is_loopback(address) && !policy_allows(security.policy, address)) {
      send_error(response, 403, "ip_denied",
                 "this address is not allowed by the server policy");
      return;
    }
    next();
  });

  app.get("/healthz", [&runtime](uvcpp::uvcpp_web_request&,
                                 uvcpp::uvcpp_web_response& response,
                                 uvcpp::uvcpp_web_next next) {
    runtime.executor().read(
        runtime.admin_app().loop(),
        [](uvcpp::uvcpp_db_client& client, std::string&) {
          return uvcpp::uvcpp_db_ok(client.ping());
        },
        [&response, next](bool ok, const std::string&) {
          json body;
          body["status"] = ok ? "ok" : "degraded";
          body["service"] = MOLINK_SERVER_PRODUCT;
          body["version"] = MOLINK_SERVER_VERSION;
          send_json(response, ok ? 200 : 503, body);
          next();
        });
  });

  app.get("/api/admin/setup",
          [&runtime](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& response,
                     uvcpp::uvcpp_web_next next) {
            auto state = std::make_shared<std::pair<bool, bool>>(false, false);
            runtime.executor().read(
                runtime.admin_app().loop(),
                [&runtime, state](uvcpp::uvcpp_db_client& client,
                                  std::string& error) {
                  std::size_t count = 0;
                  if (!runtime.users().count_all(client, runtime.db().dialect(),
                                                 count, error)) {
                    return false;
                  }
                  state->first = true;
                  state->second = count == 0;
                  return true;
                },
                [&response, next, state](bool ok, const std::string&) {
                  if (!ok || !state->first) {
                    send_error(response, 503, "database_unavailable",
                               "the database is unavailable");
                    next();
                    return;
                  }
                  json body;
                  body["needs_setup"] = state->second;
                  body["version"] = MOLINK_SERVER_VERSION;
                  send_json(response, 200, body);
                  next();
                });
          });

  app.get("/api/admin/session",
          [&runtime](uvcpp::uvcpp_web_request& request,
                     uvcpp::uvcpp_web_response& response,
                     uvcpp::uvcpp_web_next next) {
            const std::string raw = cookie_value(request, kSessionCookie);
            const security_settings security = runtime.settings().security();
            const bool secure = request_secure(runtime, request, security);
            if (raw.empty()) {
              auto state = std::make_shared<std::pair<bool, bool>>(false, false);
              runtime.executor().read(
                  runtime.admin_app().loop(),
                  [&runtime, state](uvcpp::uvcpp_db_client& client,
                                    std::string& error) {
                    std::size_t count = 0;
                    if (!runtime.users().count_all(
                            client, runtime.db().dialect(), count, error)) {
                      return false;
                    }
                    state->first = true;
                    state->second = count == 0;
                    return true;
                  },
                  [&response, next, state](bool ok, const std::string&) {
                    json body;
                    body["authenticated"] = false;
                    body["needs_setup"] = ok && state->first ? state->second : false;
                    body["version"] = MOLINK_SERVER_VERSION;
                    send_json(response, ok && state->first ? 200 : 503, body);
                    next();
                  });
              return;
            }
            auto state = std::make_shared<json>();
            runtime.executor().write(
                runtime.admin_app().loop(),
                [&runtime, raw, state](uvcpp::uvcpp_db_client& client,
                                       std::string&) {
                  admin_user user;
                  session_record session;
                  auth_error error;
                  if (!runtime.auth().validate_session(
                          client, runtime.db().dialect(), raw, user, session,
                          error)) {
                    (*state)["authenticated"] = false;
                    return false;
                  }
                  (*state)["authenticated"] = true;
                  (*state)["user"] = user_json(user);
                  (*state)["expires_at_ms"] = session.expires_ms;
                  return true;
                },
                [&response, next, state, security, secure, &request](
                    bool ok, const std::string&) {
                  if (!ok || !state->contains("authenticated") ||
                      !(*state)["authenticated"].get<bool>()) {
                    json body;
                    body["authenticated"] = false;
                    body["needs_setup"] = false;
                    body["version"] = MOLINK_SERVER_VERSION;
                    send_json(response, 200, body);
                    next();
                    return;
                  }
                  std::string csrf = cookie_value(request, kCsrfCookie);
                  if (csrf.empty()) {
                    csrf = random_hex(32);
                    response.add_header(
                        "Set-Cookie",
                        make_cookie(kCsrfCookie, csrf,
                                    security.session.absolute_seconds, false,
                                    secure));
                  }
                  (*state)["csrf_token"] = csrf;
                  (*state)["version"] = MOLINK_SERVER_VERSION;
                  send_json(response, 200, *state);
                  next();
                });
          });

  app.post("/api/admin/setup",
           [&runtime](uvcpp::uvcpp_web_request& request,
                      uvcpp::uvcpp_web_response& response,
                      uvcpp::uvcpp_web_next next) {
             const security_settings security = runtime.settings().security();
             const bool secure = request_secure(runtime, request, security);
             if (!origin_ok(request, secure)) {
               send_error(response, 403, "origin_rejected",
                          "the request origin is not allowed");
               return;
             }
             json body;
             std::string error;
             if (!parse_json_body(request, body, error)) {
               send_error(response, 400, "bad_request", error);
               return;
             }
             ip_addr address;
             effective_ip(request, security.trusted_proxies, address);
             const std::string ip = canonical_ip(address);
             if (!is_loopback(address)) {
               const std::string expected = runtime.config().setup_token;
               const std::string supplied =
                   body_string(body, "setup_token").empty()
                       ? request.header("X-MoLink-Setup-Token", "")
                       : body_string(body, "setup_token");
               if (expected.empty() ||
                   !constant_time_equals(expected, supplied)) {
                 send_error(response, 403, "setup_token_invalid",
                            "a valid one-time setup token is required");
                 return;
               }
             }
             int retry = 0;
             if (!runtime.limiter().allow(
                     "setup:" + ip,
                     static_cast<double>(security.limits.setup_per_10min) / 600.0,
                     2, epoch_ms(), &retry)) {
               send_error(response, 429, "rate_limited", "too many setup attempts");
               return;
             }
             const std::string username = body_string(body, "username");
             const std::string display_name = body_string(body, "display_name");
             const std::string password = body_string(body, "password");
             auto result = std::make_shared<login_result>();
             auto auth_err = std::make_shared<auth_error>();
             runtime.executor().write(
                 runtime.admin_app().loop(),
                 [&runtime, username, display_name, password, ip, result,
                  auth_err](uvcpp::uvcpp_db_client& client, std::string&) {
                   if (!runtime.auth().create_first_admin(
                           client, runtime.db().dialect(), username,
                           display_name, password, ip, "", *result,
                           *auth_err)) {
                     return false;
                   }
                   audit_record event;
                   event.ts_ms = epoch_ms();
                   event.event = "setup.completed";
                   event.result = "ok";
                   event.actor_id = result->user_id;
                   event.actor_name = result->username;
                   event.role = result->role;
                   event.ip = ip;
                   event.details_json = "{}";
                   std::string audit_error;
                   runtime.audit_repository_ref().append(
                       client, runtime.db().dialect(), event, audit_error);
                   return true;
                 },
                 [&response, next, result, auth_err, security, secure](
                     bool ok, const std::string& error) {
                   if (!ok) {
                     if (!auth_err->ok()) {
                       http_reply reply;
                       reply_auth_error(reply, *auth_err);
                       send_reply(response, reply);
                     } else {
                       log_error("setup: %s", error.c_str());
                       send_error(response, 503, "database_unavailable",
                                  "the database is unavailable");
                     }
                     next();
                     return;
                   }
                   http_reply reply;
                   reply.status = 201;
                   json body_out;
                   body_out["user"]["username"] = result->username;
                   body_out["user"]["display_name"] = result->display_name;
                   body_out["user"]["role"] = result->role;
                   body_out["csrf_token"] = result->csrf_token;
                   body_out["expires_at_ms"] = result->expires_ms;
                   reply.body = body_out.dump();
                   set_auth_cookies(reply, *result, epoch_ms(), security, secure);
                   send_reply(response, reply);
                   next();
                 });
           });

  app.post("/api/admin/login",
           [&runtime](uvcpp::uvcpp_web_request& request,
                      uvcpp::uvcpp_web_response& response,
                      uvcpp::uvcpp_web_next next) {
             const security_settings security = runtime.settings().security();
             const bool secure = request_secure(runtime, request, security);
             if (!origin_ok(request, secure)) {
               send_error(response, 403, "origin_rejected",
                          "the request origin is not allowed");
               return;
             }
             json body;
             std::string error;
             if (!parse_json_body(request, body, error)) {
               send_error(response, 400, "bad_request", error);
               return;
             }
             ip_addr address;
             effective_ip(request, security.trusted_proxies, address);
             const std::string ip = canonical_ip(address);
             int retry = 0;
             if (!runtime.limiter().allow(
                     "admin:login:" + ip,
                     static_cast<double>(security.limits.admin_login_per_5min) /
                         300.0,
                     5, epoch_ms(), &retry)) {
               response.set_header("Retry-After", std::to_string(retry));
               json out;
               out["type"] = "error";
               out["code"] = "rate_limited";
               out["message"] = "too many login attempts";
               send_json(response, 429, out);
               return;
             }
             const std::string username = body_string(body, "username");
             const std::string password = body_string(body, "password");
             auto result = std::make_shared<login_result>();
             auto auth_err = std::make_shared<auth_error>();
             runtime.executor().write(
                 runtime.admin_app().loop(),
                 [&runtime, username, password, ip, result, auth_err](
                     uvcpp::uvcpp_db_client& client, std::string&) {
                   if (!runtime.auth().login(
                           client, runtime.db().dialect(), username, password,
                           ip, "", *result, *auth_err)) {
                     return false;
                   }
                   audit_record event;
                   event.ts_ms = epoch_ms();
                   event.event = "auth.login";
                   event.result = "ok";
                   event.actor_id = result->user_id;
                   event.actor_name = result->username;
                   event.role = result->role;
                   event.ip = ip;
                   event.details_json = "{}";
                   std::string audit_error;
                   runtime.audit_repository_ref().append(
                       client, runtime.db().dialect(), event, audit_error);
                   return true;
                 },
                 [&response, next, result, auth_err, security, secure, ip,
                  &runtime](bool ok, const std::string& error) {
                   if (!ok) {
                     if (!auth_err->ok()) {
                       runtime.audit("auth.login.failure", "failed", "", "", "",
                                     ip, "", "{}");
                       const int ban_seconds =
                           record_login_failure(ip, security, epoch_ms());
                       if (ban_seconds > 0) {
                         schedule_auto_ban(runtime, ip, ban_seconds);
                         response.set_header("Retry-After",
                                             std::to_string(ban_seconds));
                         json out;
                         out["type"] = "error";
                         out["code"] = "ip_banned";
                         out["message"] = "too many failed logins";
                         send_json(response, 429, out);
                       } else {
                         http_reply reply;
                         reply_auth_error(reply, *auth_err);
                         send_reply(response, reply);
                       }
                     } else {
                       log_error("login: %s", error.c_str());
                       send_error(response, 503, "database_unavailable",
                                  "the database is unavailable");
                     }
                     next();
                     return;
                   }
                   clear_login_failures(ip);
                   http_reply reply;
                   json body_out;
                   body_out["user"]["username"] = result->username;
                   body_out["user"]["display_name"] = result->display_name;
                   body_out["user"]["role"] = result->role;
                   body_out["csrf_token"] = result->csrf_token;
                   body_out["expires_at_ms"] = result->expires_ms;
                   reply.body = body_out.dump();
                   set_auth_cookies(reply, *result, epoch_ms(), security, secure);
                   send_reply(response, reply);
                   next();
                 });
           });

  app.post("/api/admin/logout",
           [&runtime](uvcpp::uvcpp_web_request& request,
                      uvcpp::uvcpp_web_response& response,
                      uvcpp::uvcpp_web_next next) {
             const std::string raw = cookie_value(request, kSessionCookie);
             with_admin(runtime, request, response, next, 1, true,
                        [&runtime, raw](uvcpp::uvcpp_db_client& client,
                                        const admin_user&,
                                        const session_record&,
                                        http_reply& reply) {
                          runtime.auth().logout(client, runtime.db().dialect(),
                                                raw);
                          reply.status = 204;
                          reply.set_cookies.push_back(
                              clear_cookie_header(kSessionCookie));
                          reply.set_cookies.push_back(
                              clear_cookie_header(kCsrfCookie));
                        });
           });

  app.get("/api/admin/overview",
          [&runtime](uvcpp::uvcpp_web_request& request,
                     uvcpp::uvcpp_web_response& response,
                     uvcpp::uvcpp_web_next next) {
            with_admin(runtime, request, response, next, 1, false,
                       [&runtime](uvcpp::uvcpp_db_client& client,
                                  const admin_user&, const session_record&,
                                  http_reply& reply) {
                         const server_settings server =
                             runtime.settings().server();
                         const std::uint64_t now = epoch_ms();
                         device_query query;
                         query.page_size = 1;
                         device_page all;
                         std::string error;
                         if (!runtime.devices().list(
                                 client, runtime.db().dialect(), query,
                                 static_cast<std::uint64_t>(
                                     server.offline_after_seconds) *
                                     1000u,
                                 now, all, error)) {
                           reply_error(reply, 503, "database_unavailable", error);
                           return;
                         }
                         query.online = 1;
                         device_page online_page;
                         if (!runtime.devices().list(
                                 client, runtime.db().dialect(), query,
                                 static_cast<std::uint64_t>(
                                     server.offline_after_seconds) *
                                     1000u,
                                 now, online_page, error)) {
                           reply_error(reply, 503, "database_unavailable", error);
                           return;
                         }
                         std::size_t users = 0;
                         runtime.users().count_all(client, runtime.db().dialect(),
                                                   users, error);
                         std::vector<ban_record> bans;
                         std::size_t ban_total = 0;
                         runtime.bans_repository().list(
                             client, runtime.db().dialect(), 0, 1, bans,
                             ban_total, error);
                         uvcpp::uvcpp_db_table schema;
                         int schema_version = 0;
                         const auto schema_status = client.query(
                             "SELECT COALESCE(MAX(version), 0) AS v FROM "
                             "schema_migrations",
                             &schema);
                         if (uvcpp::uvcpp_db_ok(schema_status)) {
                           schema_version =
                               static_cast<int>(schema.value("v").to_int64());
                         }
                         json body;
                         body["version"] = MOLINK_SERVER_VERSION;
                         body["uptime_ms"] = now - runtime.started_ms();
                         body["started_at_ms"] = runtime.started_ms();
                         body["device_api"]["host"] = runtime.config().device.host;
                         body["device_api"]["port"] = runtime.config().device.port;
                         body["device_api"]["tls"] = runtime.device_tls();
                         body["admin_api"]["host"] = runtime.config().admin.host;
                         body["admin_api"]["port"] = runtime.config().admin.port;
                         body["admin_api"]["tls"] = runtime.admin_tls();
                         body["devices"]["total"] = all.total;
                         body["devices"]["online"] = online_page.total;
                         body["devices"]["offline"] =
                             all.total >= online_page.total
                                 ? all.total - online_page.total
                                 : 0;
                         body["users"]["total"] = users;
                         body["security"]["active_bans"] = ban_total;
                         body["database"]["driver"] =
                             db_driver_name(runtime.db().dialect().driver());
                         body["database"]["schema_version"] = schema_version;
                         body["relay"]["supported"] = false;
                         reply.body = body.dump();
                       });
          });

  app.get("/api/admin/devices",
          [&runtime](uvcpp::uvcpp_web_request& request,
                     uvcpp::uvcpp_web_response& response,
                     uvcpp::uvcpp_web_next next) {
            const std::size_t page = page_number(request, "page", 1, 1000000);
            const std::size_t page_size =
                std::max<std::size_t>(1, page_number(request, "page_size", 50, 200));
            const std::string* search = request.query("q");
            const std::string* online = request.query("online");
            const int online_filter =
                online == nullptr ? -1 : (*online == "1" ? 1 : (*online == "0" ? 0 : -1));
            device_query query;
            query.page = page;
            query.page_size = page_size;
            query.search = search == nullptr ? std::string() : *search;
            query.online = online_filter;
            query.sort = request.query("sort") == nullptr ? "last_seen"
                                                          : *request.query("sort");
            with_admin(runtime, request, response, next, 1, false,
                       [&runtime, query](uvcpp::uvcpp_db_client& client,
                                         const admin_user&,
                                         const session_record&,
                                         http_reply& reply) {
                         const server_settings server =
                             runtime.settings().server();
                         device_page result;
                         std::string error;
                         if (!runtime.devices().list(
                                 client, runtime.db().dialect(), query,
                                 static_cast<std::uint64_t>(
                                     server.offline_after_seconds) *
                                     1000u,
                                 epoch_ms(), result, error)) {
                           reply_error(reply, 503, "database_unavailable", error);
                           return;
                         }
                         json body;
                         body["total"] = result.total;
                         body["page"] = query.page;
                         body["page_size"] = query.page_size;
                         body["items"] = json::array();
                         for (const device_record& record : result.items) {
                           body["items"].push_back(device_json(
                               record, server.offline_after_seconds,
                               epoch_ms()));
                         }
                         reply.body = body.dump();
                       });
          });

  app.post("/api/admin/devices/:id/revoke",
           [&runtime](uvcpp::uvcpp_web_request& request,
                      uvcpp::uvcpp_web_response& response,
                      uvcpp::uvcpp_web_next next) {
             const std::string* id_param = request.param("id");
             const std::string id = id_param == nullptr ? std::string() : *id_param;
             with_admin(runtime, request, response, next, 2, true,
                        [&runtime, id](uvcpp::uvcpp_db_client& client,
                                       const admin_user& user,
                                       const session_record&, http_reply& reply) {
                          bool found = false;
                          if (!runtime.devices().revoke(
                                  client, runtime.db().dialect(), id, found)) {
                            reply_error(reply, 503, "database_unavailable",
                                        "the device directory is unavailable");
                            return;
                          }
                          if (!found) {
                            reply_error(reply, 404, "not_found", "unknown device");
                            return;
                          }
                          audit_record event;
                          event.ts_ms = epoch_ms();
                          event.event = "device.revoke";
                          event.result = "ok";
                          event.actor_id = user.id;
                          event.actor_name = user.username;
                          event.role = user.role;
                          event.target = id;
                          event.details_json = "{}";
                          std::string audit_error;
                          runtime.audit_repository_ref().append(
                              client, runtime.db().dialect(), event,
                              audit_error);
                          reply.status = 204;
                        });
           });

  app.get("/api/admin/users",
          [&runtime](uvcpp::uvcpp_web_request& request,
                     uvcpp::uvcpp_web_response& response,
                     uvcpp::uvcpp_web_next next) {
            with_admin(runtime, request, response, next, 3, false,
                       [&runtime](uvcpp::uvcpp_db_client& client,
                                  const admin_user&, const session_record&,
                                  http_reply& reply) {
                         std::vector<admin_user> users;
                         std::string error;
                         if (!runtime.users().list(client, runtime.db().dialect(),
                                                   users, error)) {
                           reply_error(reply, 503, "database_unavailable", error);
                           return;
                         }
                         json body;
                         body["items"] = json::array();
                         for (const admin_user& user : users) {
                           body["items"].push_back(user_json(user));
                         }
                         reply.body = body.dump();
                       });
          });

  app.post("/api/admin/users",
           [&runtime](uvcpp::uvcpp_web_request& request,
                      uvcpp::uvcpp_web_response& response,
                      uvcpp::uvcpp_web_next next) {
             json body;
             std::string error;
             if (!parse_json_body(request, body, error)) {
               send_error(response, 400, "bad_request", error);
               return;
             }
             const std::string username = body_string(body, "username");
             const std::string display_name = body_string(body, "display_name");
             const std::string role = body_string(body, "role");
             const std::string password = body_string(body, "password");
             with_admin(runtime, request, response, next, 3, true,
                        [&runtime, username, display_name, role, password](
                            uvcpp::uvcpp_db_client& client,
                            const admin_user& actor,
                            const session_record&, http_reply& reply) {
                          admin_user created;
                          auth_error error;
                          if (!runtime.auth().create_user(
                                  client, runtime.db().dialect(), username,
                                  display_name, role, password, created, error)) {
                            reply_auth_error(reply, error);
                            return;
                          }
                          append_audit(runtime, client, actor, "user.create",
                                       created.username);
                          reply.status = 201;
                          reply.body = user_json(created).dump();
                        });
           });

  app.patch("/api/admin/users/:id",
            [&runtime](uvcpp::uvcpp_web_request& request,
                       uvcpp::uvcpp_web_response& response,
                       uvcpp::uvcpp_web_next next) {
              json body;
              std::string error;
              if (!parse_json_body(request, body, error)) {
                send_error(response, 400, "bad_request", error);
                return;
              }
              const std::string* id_param = request.param("id");
              const std::string id = id_param == nullptr ? std::string() : *id_param;
              const std::string display_name = body_string(body, "display_name");
              const std::string role = body_string(body, "role");
              const bool disabled = body_bool(body, "disabled", false);
              with_admin(runtime, request, response, next, 3, true,
                         [&runtime, id, display_name, role, disabled](
                             uvcpp::uvcpp_db_client& client,
                             const admin_user& actor, const session_record&,
                             http_reply& reply) {
                           auth_error error;
                           if (!runtime.auth().update_user(
                                   client, runtime.db().dialect(), actor, id,
                                   display_name, role, disabled, error)) {
                             reply_auth_error(reply, error);
                             return;
                           }
                           append_audit(runtime, client, actor, "user.update",
                                        id);
                           reply.status = 204;
                         });
            });

  app.del("/api/admin/users/:id",
          [&runtime](uvcpp::uvcpp_web_request& request,
                     uvcpp::uvcpp_web_response& response,
                     uvcpp::uvcpp_web_next next) {
            const std::string* id_param = request.param("id");
            const std::string id = id_param == nullptr ? std::string() : *id_param;
            with_admin(runtime, request, response, next, 3, true,
                       [&runtime, id](uvcpp::uvcpp_db_client& client,
                                      const admin_user& actor,
                                      const session_record&, http_reply& reply) {
                         auth_error error;
                         if (!runtime.auth().delete_user(
                                 client, runtime.db().dialect(), actor, id,
                                 error)) {
                           reply_auth_error(reply, error);
                           return;
                         }
                         append_audit(runtime, client, actor, "user.delete", id);
                         reply.status = 204;
                       });
          });

  app.post("/api/admin/users/:id/password",
           [&runtime](uvcpp::uvcpp_web_request& request,
                      uvcpp::uvcpp_web_response& response,
                      uvcpp::uvcpp_web_next next) {
             json body;
             std::string error;
             if (!parse_json_body(request, body, error)) {
               send_error(response, 400, "bad_request", error);
               return;
             }
             const std::string* id_param = request.param("id");
             const std::string id = id_param == nullptr ? std::string() : *id_param;
             const std::string new_password = body_string(body, "new_password");
             with_admin(runtime, request, response, next, 3, true,
                        [&runtime, id, new_password](
                            uvcpp::uvcpp_db_client& client,
                            const admin_user& actor, const session_record&,
                            http_reply& reply) {
                          auth_error error;
                          if (!runtime.auth().reset_password(
                                  client, runtime.db().dialect(), id,
                                  new_password, error)) {
                            reply_auth_error(reply, error);
                            return;
                          }
                          append_audit(runtime, client, actor,
                                       "user.password", id);
                          reply.status = 204;
                        });
           });

  app.post("/api/admin/me/password",
           [&runtime](uvcpp::uvcpp_web_request& request,
                      uvcpp::uvcpp_web_response& response,
                      uvcpp::uvcpp_web_next next) {
             json body;
             std::string error;
             if (!parse_json_body(request, body, error)) {
               send_error(response, 400, "bad_request", error);
               return;
             }
             const std::string current = body_string(body, "current_password");
             const std::string next_password = body_string(body, "new_password");
             with_admin(runtime, request, response, next, 1, true,
                        [&runtime, current, next_password](
                            uvcpp::uvcpp_db_client& client,
                            const admin_user& user, const session_record& session,
                            http_reply& reply) {
                          auth_error error;
                          if (!runtime.auth().change_password(
                                  client, runtime.db().dialect(), user, current,
                                  next_password, error)) {
                            reply_auth_error(reply, error);
                            return;
                          }
                          runtime.sessions().remove_for_user(
                              client, runtime.db().dialect(), user.id,
                              session.id);
                          append_audit(runtime, client, user, "auth.password",
                                       user.username);
                          reply.status = 204;
                        });
           });

  app.get("/api/admin/security",
          [&runtime](uvcpp::uvcpp_web_request& request,
                     uvcpp::uvcpp_web_response& response,
                     uvcpp::uvcpp_web_next next) {
            with_admin(runtime, request, response, next, 2, false,
                       [&runtime](uvcpp::uvcpp_db_client& client,
                                  const admin_user&, const session_record&,
                                  http_reply& reply) {
                         std::string value;
                         bool found = false;
                         std::string error;
                         if (!runtime.settings_repository_ref().get(
                                 client, runtime.db().dialect(), "security",
                                 value, found, error)) {
                           reply_error(reply, 503, "database_unavailable", error);
                           return;
                         }
                         reply.body = found ? value : "{}";
                       });
          });

  app.put("/api/admin/security",
          [&runtime](uvcpp::uvcpp_web_request& request,
                     uvcpp::uvcpp_web_response& response,
                     uvcpp::uvcpp_web_next next) {
            json body;
            std::string error;
            if (!parse_json_body(request, body, error)) {
              send_error(response, 400, "bad_request", error);
              return;
            }
            const std::string document = body.dump();
            with_admin(runtime, request, response, next, 3, true,
                       [&runtime, document](uvcpp::uvcpp_db_client& client,
                                            const admin_user& user,
                                            const session_record&,
                                            http_reply& reply) {
                         std::string error;
                         if (!runtime.settings().save_security(
                                 client, runtime.db().dialect(),
                                 runtime.settings_repository_ref(), document,
                                 error)) {
                           reply_error(reply, 400, "validation_error", error);
                           return;
                         }
                         std::int64_t revision = 0;
                         runtime.settings_repository_ref().bump_revision(
                             client, runtime.db().dialect(), revision, error);
                         audit_record event;
                         event.ts_ms = epoch_ms();
                         event.event = "settings.security";
                         event.result = "ok";
                         event.actor_id = user.id;
                         event.actor_name = user.username;
                         event.role = user.role;
                         event.details_json = "{}";
                         runtime.audit_repository_ref().append(
                             client, runtime.db().dialect(), event, error);
                         reply.body = document;
                       });
          });

  app.get("/api/admin/settings/server",
          [&runtime](uvcpp::uvcpp_web_request& request,
                     uvcpp::uvcpp_web_response& response,
                     uvcpp::uvcpp_web_next next) {
            with_admin(runtime, request, response, next, 1, false,
                       [&runtime](uvcpp::uvcpp_db_client& client,
                                  const admin_user&, const session_record&,
                                  http_reply& reply) {
                         std::string value;
                         bool found = false;
                         std::string error;
                         runtime.settings_repository_ref().get(
                             client, runtime.db().dialect(), "server", value,
                             found, error);
                         reply.body = found ? value : "{}";
                       });
          });

  app.put("/api/admin/settings/server",
          [&runtime](uvcpp::uvcpp_web_request& request,
                     uvcpp::uvcpp_web_response& response,
                     uvcpp::uvcpp_web_next next) {
            json body;
            std::string error;
            if (!parse_json_body(request, body, error)) {
              send_error(response, 400, "bad_request", error);
              return;
            }
            const std::string document = body.dump();
            with_admin(runtime, request, response, next, 3, true,
                       [&runtime, document](uvcpp::uvcpp_db_client& client,
                                            const admin_user& user,
                                            const session_record&,
                                            http_reply& reply) {
                         std::string error;
                         if (!runtime.settings().save_server(
                                 client, runtime.db().dialect(),
                                 runtime.settings_repository_ref(), document,
                                 error)) {
                           reply_error(reply, 400, "validation_error", error);
                           return;
                         }
                         std::int64_t revision = 0;
                         runtime.settings_repository_ref().bump_revision(
                             client, runtime.db().dialect(), revision, error);
                         audit_record event;
                         event.ts_ms = epoch_ms();
                         event.event = "settings.server";
                         event.result = "ok";
                         event.actor_id = user.id;
                         event.actor_name = user.username;
                         event.role = user.role;
                         event.details_json = "{}";
                         runtime.audit_repository_ref().append(
                             client, runtime.db().dialect(), event, error);
                         reply.body = document;
                       });
          });

  app.get("/api/admin/bans",
          [&runtime](uvcpp::uvcpp_web_request& request,
                     uvcpp::uvcpp_web_response& response,
                     uvcpp::uvcpp_web_next next) {
            const std::size_t page = page_number(request, "page", 1, 1000000);
            const std::size_t page_size =
                std::max<std::size_t>(1, page_number(request, "page_size", 50, 200));
            with_admin(runtime, request, response, next, 2, false,
                       [&runtime, page, page_size](uvcpp::uvcpp_db_client& client,
                                                   const admin_user&,
                                                   const session_record&,
                                                   http_reply& reply) {
                         std::vector<ban_record> bans;
                         std::size_t total = 0;
                         std::string error;
                         if (!runtime.bans_repository().list(
                                 client, runtime.db().dialect(),
                                 (page - 1) * page_size, page_size, bans, total,
                                 error)) {
                           reply_error(reply, 503, "database_unavailable", error);
                           return;
                         }
                         json body;
                         body["total"] = total;
                         body["items"] = json::array();
                         for (const ban_record& ban : bans) {
                           json item;
                           item["id"] = ban.id;
                           item["target"] = ban.target;
                           item["kind"] = ban.target_kind;
                           item["reason"] = ban.reason;
                           item["source"] = ban.source;
                           item["created_ms"] = ban.created_ms;
                           item["expires_ms"] = ban.expires_ms;
                           item["created_by"] = ban.created_by;
                           item["note"] = ban.note;
                           body["items"].push_back(item);
                         }
                         reply.body = body.dump();
                       });
          });

  app.post("/api/admin/bans",
           [&runtime](uvcpp::uvcpp_web_request& request,
                      uvcpp::uvcpp_web_response& response,
                      uvcpp::uvcpp_web_next next) {
             json body;
             std::string error;
             if (!parse_json_body(request, body, error)) {
               send_error(response, 400, "bad_request", error);
               return;
             }
             const std::string target = body_string(body, "target");
             const std::string reason = body_string(body, "reason");
             const std::string note = body_string(body, "note");
             const int duration = body_int(body, "duration_seconds", 3600);
             cidr_block block;
             ip_addr single;
             const bool is_cidr = target.find('/') != std::string::npos;
             if (is_cidr) {
               if (!parse_cidr(target, block, &error)) {
                 send_error(response, 400, "validation_error", error);
                 return;
               }
             } else if (!parse_ip(target, single, &error)) {
               send_error(response, 400, "validation_error", error);
               return;
             }
             with_admin(runtime, request, response, next, 2, true,
                        [&runtime, target, reason, note, duration, is_cidr, block,
                         single](uvcpp::uvcpp_db_client& client,
                                 const admin_user& user, const session_record&,
                                 http_reply& reply) {
                          if (duration == 0 && role_rank(user.role) < 3) {
                            reply_error(reply, 403, "forbidden",
                                        "only an administrator can create a permanent ban");
                            return;
                          }
                          const std::uint64_t now = epoch_ms();
                          ban_record ban;
                          ban.id = "ban_" + random_hex(12);
                          ban.target = target;
                          ban.target_kind = is_cidr ? "cidr" : "ip";
                          ban.reason = reason;
                          ban.source = "manual";
                          ban.created_ms = now;
                          ban.expires_ms =
                              duration > 0
                                  ? now + static_cast<std::uint64_t>(duration) *
                                              1000u
                                  : 0;
                          ban.created_by = user.username;
                          ban.note = note;
                          std::string error;
                          if (!runtime.bans_repository().add(
                                  client, runtime.db().dialect(), ban, error)) {
                            reply_error(reply, 409, "conflict", error);
                            return;
                          }
                          runtime.bans().add(ban);
                          append_audit(runtime, client, user, "ban.add",
                                       ban.target);
                          reply.status = 201;
                          json out;
                          out["id"] = ban.id;
                          out["target"] = ban.target;
                          out["expires_ms"] = ban.expires_ms;
                          reply.body = out.dump();
                        });
           });

  app.del("/api/admin/bans/:id",
          [&runtime](uvcpp::uvcpp_web_request& request,
                     uvcpp::uvcpp_web_response& response,
                     uvcpp::uvcpp_web_next next) {
            const std::string* id_param = request.param("id");
            const std::string id = id_param == nullptr ? std::string() : *id_param;
            with_admin(runtime, request, response, next, 2, true,
                       [&runtime, id](uvcpp::uvcpp_db_client& client,
                                      const admin_user& user,
                                      const session_record&, http_reply& reply) {
                         bool found = false;
                         std::string error;
                         if (!runtime.bans_repository().remove(
                                 client, runtime.db().dialect(), id, found)) {
                           reply_error(reply, 503, "database_unavailable", error);
                           return;
                         }
                         if (!found) {
                           reply_error(reply, 404, "not_found", "unknown ban");
                           return;
                         }
                         runtime.bans().remove(id);
                         append_audit(runtime, client, user, "ban.remove", id);
                         reply.status = 204;
                       });
          });

  app.get("/api/admin/audit",
          [&runtime](uvcpp::uvcpp_web_request& request,
                     uvcpp::uvcpp_web_response& response,
                     uvcpp::uvcpp_web_next next) {
            const std::size_t page = page_number(request, "page", 1, 1000000);
            const std::size_t page_size =
                std::max<std::size_t>(1, page_number(request, "page_size", 50, 200));
            with_admin(runtime, request, response, next, 2, false,
                       [&runtime, page, page_size](uvcpp::uvcpp_db_client& client,
                                                   const admin_user&,
                                                   const session_record&,
                                                   http_reply& reply) {
                         std::vector<audit_record> events;
                         std::size_t total = 0;
                         std::string error;
                         if (!runtime.audit_repository_ref().list(
                                 client, runtime.db().dialect(),
                                 (page - 1) * page_size, page_size, events,
                                 total, error)) {
                           reply_error(reply, 503, "database_unavailable", error);
                           return;
                         }
                         json body;
                         body["total"] = total;
                         body["items"] = json::array();
                         for (const audit_record& event : events) {
                           json item;
                           item["id"] = event.id;
                           item["ts_ms"] = event.ts_ms;
                           item["event"] = event.event;
                           item["result"] = event.result;
                           item["actor_id"] = event.actor_id;
                           item["actor_name"] = event.actor_name;
                           item["role"] = event.role;
                           item["ip"] = event.ip;
                           item["target"] = event.target;
                           item["details"] = event.details_json;
                           body["items"].push_back(item);
                         }
                         reply.body = body.dump();
                       });
          });

  register_admin_ui(app, ui_root);
}

}  // namespace molink
