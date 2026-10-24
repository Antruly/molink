#include "http_common.h"

#include <algorithm>
#include <cctype>

#include "logging.h"

namespace molink {
namespace {

bool content_type_is_json(const std::string& value) {
  std::string lower = value;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower.find("application/json") != std::string::npos;
}

}  // namespace

void send_reply(uvcpp::uvcpp_web_response& response, const http_reply& reply) {
  response.status(reply.status);
  for (const auto& header : reply.headers) {
    response.set_header(header.first, header.second);
  }
  for (const std::string& cookie : reply.set_cookies) {
    response.add_header("Set-Cookie", cookie);
  }
  if (reply.status == 204 || reply.status == 304) {
    response.end();
    return;
  }
  if (reply.content_type.find("application/json") != std::string::npos) {
    response.set_header("Content-Type", reply.content_type);
    response.json_str(reply.body);
  } else {
    response.set_header("Content-Type", reply.content_type);
    response.text(reply.body);
  }
  response.end();
}

void send_json(uvcpp::uvcpp_web_response& response, int status,
               const nlohmann::json& body) {
  response.status(status);
  response.set_header("Cache-Control", "no-store");
  response.json_str(body.dump());
  response.end();
}

void send_error(uvcpp::uvcpp_web_response& response, int status,
                const std::string& code, const std::string& message) {
  nlohmann::json body;
  body["type"] = "error";
  body["code"] = code;
  body["message"] = message;
  send_json(response, status, body);
}

bool parse_json_body(const uvcpp::uvcpp_web_request& request,
                     nlohmann::json& out, std::string& error) {
  const std::string type = request.header("Content-Type", "");
  if (!content_type_is_json(type)) {
    error = "Content-Type must be application/json";
    return false;
  }
  const std::string body = request.body_str();
  if (body.empty()) {
    error = "request body is empty";
    return false;
  }
  try {
    out = nlohmann::json::parse(body);
  } catch (const std::exception& e) {
    error = std::string("invalid JSON body: ") + e.what();
    return false;
  }
  if (!out.is_object()) {
    error = "request body must be a JSON object";
    return false;
  }
  return true;
}

std::string cookie_value(const uvcpp::uvcpp_web_request& request,
                         const std::string& name) {
  const std::string* value = request.cookie(name);
  return value == nullptr ? std::string() : *value;
}

std::string make_cookie(const std::string& name, const std::string& value,
                        int max_age_seconds, bool http_only, bool secure) {
  std::string cookie = name + "=" + value + "; Path=/; SameSite=Strict";
  if (max_age_seconds >= 0) {
    cookie += "; Max-Age=" + std::to_string(max_age_seconds);
  }
  if (http_only) cookie += "; HttpOnly";
  if (secure) cookie += "; Secure";
  return cookie;
}

std::string clear_cookie_header(const std::string& name) {
  return name + "=; Path=/; Max-Age=0; SameSite=Strict";
}

bool effective_ip(const uvcpp::uvcpp_web_request& request,
                  const std::vector<cidr_block>& trusted_proxies,
                  ip_addr& out) {
  if (!trusted_proxies.empty()) {
    const std::string forwarded = request.header("X-Forwarded-For", "");
    if (!forwarded.empty()) {
      if (resolve_client_ip(request.peer_ip(), forwarded, trusted_proxies,
                            out)) {
        return true;
      }
      log_warn("ignoring a malformed X-Forwarded-For from %s",
               request.peer_ip().c_str());
    }
  }
  return parse_ip(request.peer_ip(), out, nullptr);
}

bool is_loopback_peer(const std::string& peer) {
  ip_addr address;
  return parse_ip(peer, address, nullptr) && is_loopback(address);
}

bool request_is_https(const uvcpp::uvcpp_web_request& request, bool tls_enabled,
                      bool trust_forwarded_proto) {
  if (tls_enabled) return true;
  if (!trust_forwarded_proto) return false;
  const std::string proto = request.header("X-Forwarded-Proto", "");
  return proto == "https";
}

void apply_security_headers(uvcpp::uvcpp_web_response& response) {
  response.set_header("X-Content-Type-Options", "nosniff");
  response.set_header("X-Frame-Options", "DENY");
  response.set_header("Referrer-Policy", "no-referrer");
  response.set_header("Permissions-Policy",
                      "camera=(), microphone=(), geolocation=()");
  response.set_header(
      "Content-Security-Policy",
      "default-src 'self'; script-src 'self'; style-src 'self' "
      "'unsafe-inline'; img-src 'self' data:; connect-src 'self'; "
      "frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
}

std::string request_origin(const uvcpp::uvcpp_web_request& request) {
  return request.header("Origin", "");
}

}  // namespace molink
