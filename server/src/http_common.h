#pragma once

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <webapp/uvcpp_web_app.h>

#include "security/ip.h"

namespace molink {

struct http_reply {
  int status = 200;
  std::string body;
  std::string content_type = "application/json; charset=utf-8";
  std::vector<std::pair<std::string, std::string>> headers;
  std::vector<std::string> set_cookies;
};

void send_reply(uvcpp::uvcpp_web_response& response, const http_reply& reply);
void send_json(uvcpp::uvcpp_web_response& response, int status,
               const nlohmann::json& body);
void send_error(uvcpp::uvcpp_web_response& response, int status,
                const std::string& code, const std::string& message);

bool parse_json_body(const uvcpp::uvcpp_web_request& request,
                     nlohmann::json& out, std::string& error);

std::string cookie_value(const uvcpp::uvcpp_web_request& request,
                         const std::string& name);
std::string make_cookie(const std::string& name, const std::string& value,
                        int max_age_seconds, bool http_only, bool secure);
std::string clear_cookie_header(const std::string& name);

/// Resolve the peer through trusted X-Forwarded-For hops. Returns false for a
/// malformed header or peer; callers fall back to the raw peer address.
bool effective_ip(const uvcpp::uvcpp_web_request& request,
                  const std::vector<cidr_block>& trusted_proxies,
                  ip_addr& out);

bool is_loopback_peer(const std::string& peer);
bool request_is_https(const uvcpp::uvcpp_web_request& request, bool tls_enabled,
                      bool trust_forwarded_proto);

void apply_security_headers(uvcpp::uvcpp_web_response& response);

std::string request_origin(const uvcpp::uvcpp_web_request& request);

}  // namespace molink
