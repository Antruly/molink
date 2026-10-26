#include "server_client.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>
#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>

// directory_address() opens a probe socket; keep the platform headers local to
// this file so the macOS build gets close()/getaddrinfo() declarations too.
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// The config macro comes from the web headers above. Windows dependency builds
// may omit OpenSSL entirely, in which case HTTPS reports a clear error.
#if UVCPP_OPENSSL_ENABLE
#include <ssl/uvcpp_ssl_context.h>
#endif

#include "auth_state.h"
#include "identity.h"
#include "settings.h"
#include "util.h"

namespace molink {
namespace {

using nlohmann::json;

constexpr uint64_t kHeartbeatMs = 30000;
constexpr uint64_t kOnlineWindowMs = 75000;
constexpr int kHttpTimeoutMs = 5000;

uint64_t epoch_ms() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch())
          .count());
}

std::string trim(std::string value) {
  while (!value.empty() &&
         (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' ||
          value.back() == '\n')) {
    value.pop_back();
  }
  std::size_t start = 0;
  while (start < value.size() &&
         (value[start] == ' ' || value[start] == '\t')) {
    ++start;
  }
  return value.substr(start);
}

struct parsed_url {
  std::string host;
  int port = 80;
  std::string base;
  bool tls = false;
};

bool parse_url(const std::string& raw, parsed_url& out, std::string& err) {
  std::string rest;
  if (raw.rfind("http://", 0) == 0) {
    out.port = 80;
    rest = raw.substr(7);
  } else if (raw.rfind("https://", 0) == 0) {
    out.port = 443;
    out.tls = true;
    rest = raw.substr(8);
  } else {
    err = "server URL must start with http:// or https://";
    return false;
  }
  while (!rest.empty() && rest.back() == '/') rest.pop_back();
  if (rest.empty()) {
    err = "server URL has no host";
    return false;
  }
  const std::size_t slash = rest.find('/');
  const std::string authority =
      slash == std::string::npos ? rest : rest.substr(0, slash);
  out.base = slash == std::string::npos ? std::string() : rest.substr(slash);
  std::string port_text;
  if (!authority.empty() && authority.front() == '[') {
    const std::size_t close = authority.find(']');
    if (close == std::string::npos) {
      err = "server URL has an invalid IPv6 host";
      return false;
    }
    out.host = authority.substr(1, close - 1);
    if (close + 1 < authority.size() && authority[close + 1] == ':') {
      port_text = authority.substr(close + 2);
    }
  } else {
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string::npos &&
        authority.find(':') == colon) {
      out.host = authority.substr(0, colon);
      port_text = authority.substr(colon + 1);
    } else {
      out.host = authority;
    }
  }
  if (out.host.empty()) {
    err = "server URL has no host";
    return false;
  }
  if (!port_text.empty()) {
    try {
      out.port = std::stoi(port_text);
    } catch (...) {
      err = "server URL has an invalid port";
      return false;
    }
    if (out.port <= 0 || out.port > 65535) {
      err = "server URL has an invalid port";
      return false;
    }
  }
  return true;
}

std::string credentials_path() {
  const std::string dir = config_directory();
  if (dir.empty()) return std::string();
  return (std::filesystem::path(dir) / "server.json").string();
}

}  // namespace

server_client::server_client(std::shared_ptr<auth_state> auth,
                             std::shared_ptr<std::atomic<int>> bound_port,
                             std::string version)
    : auth_(std::move(auth)),
      bound_port_(std::move(bound_port)),
      version_(std::move(version)) {}

server_client::~server_client() { stop(); }

void server_client::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { run(); });
}

void server_client::stop() {
  if (!running_.exchange(false)) {
    if (thread_.joinable()) thread_.join();
    return;
  }
  if (thread_.joinable()) thread_.join();
}

void server_client::reconfigure() { reconfigure_.store(true); }

server_status server_client::status() const {
  server_status copy;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    copy = status_;
  }
  if (copy.registered &&
      epoch_ms() - copy.last_heartbeat_ms > kOnlineWindowMs) {
    copy.online = false;
  }
  return copy;
}

void server_client::set_state(bool registered, bool online,
                              const std::string& error) {
  std::lock_guard<std::mutex> lock(mutex_);
  status_.registered = registered;
  status_.online = online;
  status_.last_error = error;
  if (online) status_.last_heartbeat_ms = epoch_ms();
}

bool server_client::load_credentials(const std::string& url) {
  const std::string path = credentials_path();
  if (path.empty()) return false;
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  json document;
  try {
    in >> document;
  } catch (...) {
    return false;
  }
  if (!document.is_object()) return false;
  if (document.value("url", std::string()) != url) return false;
  const std::string id = document.value("device_id", std::string());
  const std::string token = document.value("token", std::string());
  const std::string display = document.value("display_id", std::string());
  const std::string domain = document.value("domain", std::string());
  const bool domain_valid = document.value("domain_valid", false);
  if (id.empty() || token.empty()) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  token_ = token;
  status_.device_id = id;
  status_.display_id = display;
  status_.domain = domain;
  status_.domain_valid = domain_valid && !domain.empty();
  status_.registered = true;
  status_.online = false;
  status_.last_heartbeat_ms = 0;
  status_.last_error.clear();
  return true;
}

void server_client::save_credentials(const std::string& url) {
  const std::string path = credentials_path();
  if (path.empty()) return;
  std::lock_guard<std::mutex> credentials_lock(credentials_mutex_);
  json document;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    document["url"] = url;
    document["device_id"] = status_.device_id;
    document["display_id"] = status_.display_id;
    document["domain"] = status_.domain;
    document["domain_valid"] = status_.domain_valid;
    document["token"] = token_;
  }
  const std::string temporary = path + ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << document.dump(2) << '\n';
    if (!out) return;
  }
  std::error_code ec;
  std::filesystem::rename(temporary, path, ec);
  if (ec) return;
#if !defined(_WIN32)
  std::filesystem::permissions(
      path,
      std::filesystem::perms::owner_read |
          std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace, ec);
#endif
}

void server_client::clear_credentials() {
  std::lock_guard<std::mutex> lock(mutex_);
  token_.clear();
  status_.registered = false;
  status_.online = false;
  status_.device_id.clear();
  status_.display_id.clear();
  status_.domain.clear();
  status_.domain_valid = false;
  status_.last_heartbeat_ms = 0;
}

bool server_client::set_domain(const std::string& action,
                               const std::string& code, std::string& err) {
  if (action != "join" && action != "rotate") {
    err = "action must be join or rotate";
    return false;
  }
  std::string id;
  std::string token;
  std::string url;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    id = status_.device_id;
    token = token_;
    url = active_url_;
  }
  if (id.empty() || token.empty() || url.empty()) {
    err = "not registered with the directory";
    return false;
  }
  json payload;
  payload["id"] = id;
  payload["token"] = token;
  payload["action"] = action;
  if (action == "join") payload["code"] = trim(code);
  std::string body;
  std::string error;
  int http_status = 0;
  if (!post(url, "/v1/devices/domain", "", payload.dump(), body, http_status,
            error)) {
    err = error;
    return false;
  }
  if (http_status == 404) {
    err = "no such domain code";
    return false;
  }
  if (http_status != 200) {
    err = "server returned HTTP " + std::to_string(http_status) +
          (body.empty() ? std::string() : ": " + body.substr(0, 200));
    return false;
  }
  try {
    const json response = json::parse(body);
    const std::string domain = response.value("domain", std::string());
    const bool valid = response.value("domain_valid", false);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      status_.domain = domain;
      status_.domain_valid = valid && !domain.empty();
    }
  } catch (...) {
    err = "the directory returned invalid JSON";
    return false;
  }
  save_credentials(url);
  return true;
}

bool server_client::list_domain_devices(std::vector<domain_peer>& out,
                                        std::string& err) {
  out.clear();
  std::string id;
  std::string token;
  std::string url;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    id = status_.device_id;
    token = token_;
    url = active_url_;
  }
  if (id.empty() || token.empty() || url.empty()) {
    err = "not registered with the directory";
    return false;
  }
  const std::string query =
      "id=" + url_encode(id) + "&token=" + url_encode(token);
  std::string body;
  std::string error;
  int http_status = 0;
  if (!get(url, "/v1/devices/domain", query, body, http_status, error)) {
    err = error;
    return false;
  }
  if (http_status == 404) {
    err = "no such domain code";
    return false;
  }
  if (http_status != 200) {
    err = "server returned HTTP " + std::to_string(http_status);
    return false;
  }
  try {
    const json response = json::parse(body);
    const std::string domain = response.value("domain", std::string());
    const auto devices = response.find("devices");
    if (devices != response.end() && devices->is_array()) {
      for (const json& item : *devices) {
        domain_peer peer;
        peer.id = item.value("id", std::string());
        peer.display_id = item.value("display_id", std::string());
        peer.name = item.value("name", std::string());
        peer.platform = item.value("platform", std::string());
        peer.avatar = item.value("avatar", std::string());
        peer.host = item.value("address", std::string());
        if (peer.host.empty()) peer.host = item.value("peer_ip", std::string());
        peer.port = item.value("port", 0);
        peer.online = item.value("online", false);
        peer.last_seen_ms =
            item.value("last_seen_ms", static_cast<uint64_t>(0));
        if (!peer.id.empty()) out.push_back(std::move(peer));
      }
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      status_.domain = domain;
      status_.domain_valid = !domain.empty();
    }
  } catch (...) {
    err = "the directory returned invalid JSON";
    return false;
  }
  return true;
}

server_client::signaling_credentials
server_client::signaling_credentials_locked() const {
  signaling_credentials out;
  out.url = active_url_;
  out.device_id = status_.device_id;
  out.token = token_;
  return out;
}

namespace {

signal_session_info parse_signal_session(const json& item) {
  signal_session_info session;
  session.connect_id = item.value("connect_id", std::string());
  const auto from = item.find("from");
  if (from != item.end() && from->is_object()) {
    session.from_id = from->value("id", std::string());
    session.from_name = from->value("name", std::string());
  }
  session.offer = item.value("offer", std::string());
  session.answer = item.value("answer", std::string());
  session.answered = item.value("answered", false);
  session.closed = item.value("closed", false);
  session.expired = item.value("expired", false);
  session.expires_ms = item.value("expires_ms", static_cast<uint64_t>(0));
  for (const char* key : {"from_candidates", "to_candidates"}) {
    const auto values = item.find(key);
    if (values == item.end() || !values->is_array()) continue;
    auto& target = std::string(key) == "from_candidates"
                       ? session.from_candidates
                       : session.to_candidates;
    for (const json& value : *values) {
      if (value.is_string()) target.push_back(value.get<std::string>());
    }
  }
  return session;
}

std::string signal_error_text(const std::string& body, int status) {
  try {
    const json response = json::parse(body);
    const std::string code = response.value("code", std::string());
    const std::string message = response.value("message", std::string());
    if (!code.empty() || !message.empty()) {
      return code + (message.empty() ? std::string() : ": " + message);
    }
  } catch (...) {
  }
  return "server returned HTTP " + std::to_string(status);
}

}  // namespace

bool server_client::signal_create(const std::string& target,
                                  const std::string& offer,
                                  const std::vector<std::string>& candidates,
                                  std::string& connect_id,
                                  std::string& err) {
  signaling_credentials credentials;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    credentials = signaling_credentials_locked();
  }
  if (credentials.url.empty() || credentials.device_id.empty() ||
      credentials.token.empty()) {
    err = "not registered with the directory";
    return false;
  }
  json payload;
  payload["id"] = credentials.device_id;
  payload["token"] = credentials.token;
  payload["target"] = target;
  payload["offer"] = offer;
  payload["candidates"] = candidates;
  std::string body;
  std::string error;
  int status = 0;
  if (!post(credentials.url, "/v1/connect", "", payload.dump(), body, status,
            error)) {
    err = error;
    return false;
  }
  if (status != 200) {
    err = signal_error_text(body, status);
    return false;
  }
  try {
    const json response = json::parse(body);
    connect_id = response.value("connect_id", std::string());
  } catch (...) {
    err = "the directory returned invalid JSON";
    return false;
  }
  if (connect_id.empty()) {
    err = "the directory did not return a connect id";
    return false;
  }
  return true;
}

bool server_client::signal_pending(std::vector<signal_session_info>& out,
                                   std::string& err) {
  out.clear();
  signaling_credentials credentials;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    credentials = signaling_credentials_locked();
  }
  if (credentials.url.empty() || credentials.device_id.empty() ||
      credentials.token.empty()) {
    err = "not registered with the directory";
    return false;
  }
  const std::string query =
      "id=" + url_encode(credentials.device_id) +
      "&token=" + url_encode(credentials.token);
  std::string body;
  std::string error;
  int status = 0;
  if (!get(credentials.url, "/v1/connect/pending", query, body, status,
           error)) {
    err = error;
    return false;
  }
  if (status != 200) {
    err = signal_error_text(body, status);
    return false;
  }
  try {
    const json response = json::parse(body);
    const auto sessions = response.find("sessions");
    if (sessions != response.end() && sessions->is_array()) {
      for (const json& item : *sessions) {
        out.push_back(parse_signal_session(item));
      }
    }
  } catch (...) {
    err = "the directory returned invalid JSON";
    return false;
  }
  return true;
}

bool server_client::signal_answer(const std::string& connect_id,
                                  const std::string& answer,
                                  const std::vector<std::string>& candidates,
                                  std::string& err) {
  signaling_credentials credentials;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    credentials = signaling_credentials_locked();
  }
  if (credentials.url.empty()) {
    err = "not registered with the directory";
    return false;
  }
  json payload;
  payload["id"] = credentials.device_id;
  payload["token"] = credentials.token;
  payload["connect_id"] = connect_id;
  payload["answer"] = answer;
  payload["candidates"] = candidates;
  std::string body;
  std::string error;
  int status = 0;
  if (!post(credentials.url, "/v1/connect/answer", "", payload.dump(), body,
            status, error)) {
    err = error;
    return false;
  }
  if (status != 200) {
    err = signal_error_text(body, status);
    return false;
  }
  return true;
}

bool server_client::signal_status(const std::string& connect_id,
                                  signal_session_info& out,
                                  std::string& err) {
  out = signal_session_info{};
  signaling_credentials credentials;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    credentials = signaling_credentials_locked();
  }
  if (credentials.url.empty()) {
    err = "not registered with the directory";
    return false;
  }
  const std::string query =
      "id=" + url_encode(credentials.device_id) +
      "&token=" + url_encode(credentials.token) +
      "&connect_id=" + url_encode(connect_id);
  std::string body;
  std::string error;
  int status = 0;
  if (!get(credentials.url, "/v1/connect/status", query, body, status,
           error)) {
    err = error;
    return false;
  }
  if (status == 404) {
    err = "connect_not_found";
    return false;
  }
  if (status != 200) {
    err = signal_error_text(body, status);
    return false;
  }
  try {
    const json response = json::parse(body);
    const auto session = response.find("session");
    if (session != response.end() && session->is_object()) {
      out = parse_signal_session(*session);
    }
  } catch (...) {
    err = "the directory returned invalid JSON";
    return false;
  }
  return true;
}

bool server_client::signal_candidates(
    const std::string& connect_id,
    const std::vector<std::string>& candidates, std::string& err) {
  signaling_credentials credentials;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    credentials = signaling_credentials_locked();
  }
  if (credentials.url.empty()) {
    err = "not registered with the directory";
    return false;
  }
  json payload;
  payload["id"] = credentials.device_id;
  payload["token"] = credentials.token;
  payload["connect_id"] = connect_id;
  payload["candidates"] = candidates;
  std::string body;
  std::string error;
  int status = 0;
  if (!post(credentials.url, "/v1/connect/candidates", "", payload.dump(),
            body, status, error)) {
    err = error;
    return false;
  }
  if (status != 200) {
    err = signal_error_text(body, status);
    return false;
  }
  return true;
}

bool server_client::signal_close(const std::string& connect_id,
                                 std::string& err) {
  signaling_credentials credentials;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    credentials = signaling_credentials_locked();
  }
  if (credentials.url.empty()) {
    err = "not registered with the directory";
    return false;
  }
  json payload;
  payload["id"] = credentials.device_id;
  payload["token"] = credentials.token;
  payload["connect_id"] = connect_id;
  std::string body;
  std::string error;
  int status = 0;
  if (!post(credentials.url, "/v1/connect/close", "", payload.dump(), body,
            status, error)) {
    err = error;
    return false;
  }
  if (status != 200) {
    err = signal_error_text(body, status);
    return false;
  }
  return true;
}

bool server_client::directory_lookup(const std::string& query,
                                     std::string& body, int& http_status,
                                     std::string& error) {
  signaling_credentials credentials;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    credentials = signaling_credentials_locked();
  }
  if (credentials.url.empty()) {
    error = "not registered with the directory";
    return false;
  }
  return get(credentials.url, "/v1/devices", query, body, http_status, error);
}

bool server_client::relay_create(const std::string& connect_id,
                                 std::string& relay_url,
                                 std::string& ca_path, std::string& error) {
  signaling_credentials credentials;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    credentials = signaling_credentials_locked();
  }
  if (credentials.url.empty() || credentials.device_id.empty() ||
      credentials.token.empty()) {
    error = "not registered with the directory";
    return false;
  }
  json payload;
  payload["id"] = credentials.device_id;
  payload["token"] = credentials.token;
  payload["connect_id"] = connect_id;
  std::string body;
  int status = 0;
  if (!post(credentials.url, "/v1/connect/relay", "", payload.dump(), body,
            status, error)) {
    return false;
  }
  if (status != 200) {
    error = signal_error_text(body, status);
    return false;
  }
  std::string relay_token;
  try {
    const json response = json::parse(body);
    relay_token = response.value("relay_token", std::string());
  } catch (...) {
    error = "the directory returned invalid JSON";
    return false;
  }
  if (relay_token.empty()) {
    error = "the directory did not return a relay token";
    return false;
  }
  std::string base = credentials.url;
  while (!base.empty() && base.back() == '/') base.pop_back();
  std::string scheme;
  if (base.rfind("https://", 0) == 0) {
    scheme = "wss://";
    base.erase(0, 8);
  } else if (base.rfind("http://", 0) == 0) {
    scheme = "ws://";
    base.erase(0, 7);
  } else {
    error = "unsupported directory URL";
    return false;
  }
  const std::size_t slash = base.find('/');
  if (slash != std::string::npos) base = base.substr(0, slash);
  relay_url = scheme + base + "/v1/relay?connect_id=" +
              url_encode(connect_id) + "&token=" + url_encode(relay_token);
  ca_path = auth_ ? effective_server_ca(auth_->settings()) : std::string();
  return true;
}

bool server_client::post(const std::string& base_url, const std::string& path,
                         const std::string& query, const std::string& payload,
                         std::string& body, int& http_status,
                         std::string& error) {
  return request(true, base_url, path, query, payload, body, http_status,
                 error);
}

bool server_client::get(const std::string& base_url, const std::string& path,
                        const std::string& query, std::string& body,
                        int& http_status, std::string& error) {
  return request(false, base_url, path, query, "", body, http_status, error);
}

bool server_client::request(bool post_method, const std::string& base_url,
                            const std::string& path, const std::string& query,
                            const std::string& payload, std::string& body,
                            int& http_status, std::string& error) {
  std::lock_guard<std::mutex> http_lock(http_mutex_);
  parsed_url parsed;
  if (!parse_url(base_url, parsed, error)) return false;
  std::string target = parsed.base + path;
  if (!query.empty()) target += "?" + query;

#if UVCPP_OPENSSL_ENABLE
  // The HTTP client keeps a raw pointer to this context. Declare it first so
  // the client is destroyed first; the other order frees the context before
  // ~uvcpp_http_client runs and has crashed intermittently.
  uvcpp::uvcpp_ssl_context tls_context(uvcpp::tls_mode::CLIENT);
#endif
  uvcpp::uvcpp_http_client client;
#if UVCPP_OPENSSL_ENABLE
  if (parsed.tls) {
    const std::string ca_path =
        auth_ ? effective_server_ca(auth_->settings()) : std::string();
    tls_context.set_verify_mode(uvcpp::tls_verify_mode::PEER_STRICT);
    if (!ca_path.empty() && !tls_context.load_ca_file(ca_path)) {
      error = "could not load the server CA file";
      return false;
    }
    client.set_ssl_context(&tls_context);
  }
#else
  if (parsed.tls) {
    error = "this build has no TLS support; use an http:// server URL";
    return false;
  }
#endif
  if (client.connect_wait(parsed.host.c_str(), parsed.port, kHttpTimeoutMs) !=
      0) {
    error = "could not connect to the server";
    return false;
  }
  auto outgoing = post_method
                      ? uvcpp::uvcpp_http_request::make_post(
                            target.c_str(), payload.c_str(), payload.size(),
                            "application/json")
                      : uvcpp::uvcpp_http_request::make_get(target.c_str());
  uvcpp::uvcpp_http_response response;
  int rc = 0;
#if UVCPP_OPENSSL_ENABLE
  if (parsed.tls) {
    rc = client.send_wait_ssl(outgoing, response, kHttpTimeoutMs);
  } else {
    rc = client.send_wait(outgoing, response, kHttpTimeoutMs);
  }
#else
  rc = client.send_wait(outgoing, response, kHttpTimeoutMs);
#endif
  if (rc != 0) {
    error = "server request failed (" + std::to_string(rc) + ")";
    return false;
  }
  http_status = static_cast<int>(response.status_code);
  body = response.body.to_string();
  return true;
}

namespace {

/// The local IPv4 address the OS uses to reach the directory. Advertising this
/// one (rather than the first interface address) matters on multi-homed hosts:
/// the connector and the directory are on the same network.
std::string directory_address(const std::string& url) {
  std::string rest = url;
  const std::size_t scheme = rest.find("://");
  if (scheme != std::string::npos) rest = rest.substr(scheme + 3);
  const std::size_t slash = rest.find('/');
  if (slash != std::string::npos) rest = rest.substr(0, slash);
  if (rest.empty()) return std::string();

  std::string host = rest;
  std::string port = "443";
  if (rest.front() == '[') {
    const std::size_t close = rest.find(']');
    if (close == std::string::npos) return std::string();
    host = rest.substr(1, close - 1);
    if (close + 2 <= rest.size() && rest[close + 1] == ':') {
      port = rest.substr(close + 2);
    }
  } else {
    const std::size_t colon = rest.rfind(':');
    if (colon != std::string::npos) {
      host = rest.substr(0, colon);
      port = rest.substr(colon + 1);
    }
  }

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* resolved = nullptr;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &resolved) != 0) {
    return std::string();
  }

  std::string result;
#if defined(_WIN32)
  SOCKET socket_handle = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_handle != INVALID_SOCKET) {
    if (::connect(socket_handle,
                  static_cast<const sockaddr*>(resolved->ai_addr),
                  static_cast<int>(resolved->ai_addrlen)) == 0) {
      sockaddr_in local{};
      int length = sizeof(local);
      if (::getsockname(socket_handle,
                        reinterpret_cast<sockaddr*>(&local), &length) == 0) {
        char text[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text)) != nullptr) {
          result = text;
        }
      }
    }
    ::closesocket(socket_handle);
  }
#else
  const int socket_handle = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_handle >= 0) {
    if (::connect(socket_handle, resolved->ai_addr, resolved->ai_addrlen) == 0) {
      sockaddr_in local{};
      socklen_t length = sizeof(local);
      if (::getsockname(socket_handle, reinterpret_cast<sockaddr*>(&local),
                        &length) == 0) {
        char text[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text)) != nullptr) {
          result = text;
        }
      }
    }
    ::close(socket_handle);
  }
#endif
  freeaddrinfo(resolved);

  if (!result.empty() && !is_loopback_peer(result)) return result;
  for (const std::string& address : local_ipv4_addresses()) {
    if (!is_loopback_peer(address)) return address;
  }
  return std::string();
}

}  // namespace

bool server_client::try_register(const std::string& url,
                                 const std::string& invite) {
  const int port = bound_port_ ? bound_port_->load() : 0;
  if (port <= 0) {
    set_state(false, false, "the core is not listening yet");
    return false;
  }
  const app_settings settings = auth_->settings();
  const std::string name =
      settings.device_name.empty() ? std::string("moLink") : settings.device_name;
  const std::string local_address = directory_address(url);
  std::string query = "name=" + url_encode(name) +
                      "&port=" + std::to_string(port) +
                      "&version=" + url_encode(version_);
  if (!local_address.empty()) {
    query += "&address=" + url_encode(local_address);
  }
  if (!invite.empty()) query += "&invite=" + url_encode(invite);

  json payload;
  payload["name"] = name;
  payload["port"] = port;
  payload["version"] = version_;
  payload["platform"] = device_platform();
  if (!local_address.empty()) payload["address"] = local_address;
  if (!settings.avatar.empty()) payload["avatar"] = settings.avatar;
  if (!invite.empty()) payload["invite"] = invite;

  std::string body;
  std::string error;
  int http_status = 0;
  if (!post(url, "/v1/devices/register", query, payload.dump(), body,
            http_status, error)) {
    set_state(false, false, error);
    return false;
  }
  if (http_status != 200) {
    set_state(false, false,
              "server returned HTTP " + std::to_string(http_status) +
                  (body.empty() ? std::string() : ": " + body.substr(0, 200)));
    return false;
  }
  std::string id;
  std::string token;
  std::string display;
  std::string domain;
  try {
    const json response = json::parse(body);
    id = response.value("device_id", std::string());
    token = response.value("token", std::string());
    display = response.value("display_id", std::string());
    domain = response.value("domain", std::string());
  } catch (...) {
    set_state(false, false, "server response was not valid JSON");
    return false;
  }
  if (id.empty() || token.empty()) {
    set_state(false, false, "server response did not contain a device token");
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    token_ = token;
    status_.device_id = id;
    status_.display_id = display;
    status_.url = url;
    status_.domain = domain;
    status_.domain_valid = !domain.empty();
    status_.last_error.clear();
  }
  active_url_ = url;
  save_credentials(url);
  set_state(true, true, "");
  std::fprintf(stderr, "molink-server: registered as %s\n", id.c_str());
  return true;
}

bool server_client::try_heartbeat() {
  const int port = bound_port_ ? bound_port_->load() : 0;
  std::string id;
  std::string token;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    id = status_.device_id;
    token = token_;
  }
  if (id.empty() || token.empty() || port <= 0) {
    clear_credentials();
    return false;
  }
  const app_settings settings = auth_->settings();
  const std::string name =
      settings.device_name.empty() ? std::string("moLink") : settings.device_name;
  const std::string local_address = directory_address(active_url_);
  std::string query =
      "id=" + url_encode(id) + "&token=" + url_encode(token) +
      "&port=" + std::to_string(port) + "&version=" + url_encode(version_) +
      "&name=" + url_encode(name);
  if (!local_address.empty()) {
    query += "&address=" + url_encode(local_address);
  }
  json payload;
  payload["id"] = id;
  payload["token"] = token;
  payload["port"] = port;
  payload["version"] = version_;
  payload["name"] = name;
  payload["platform"] = device_platform();
  if (!local_address.empty()) payload["address"] = local_address;
  if (!settings.avatar.empty()) payload["avatar"] = settings.avatar;
  std::string body;
  std::string error;
  int http_status = 0;
  if (!post(active_url_, "/v1/devices/heartbeat", query, payload.dump(), body,
            http_status, error)) {
    set_state(true, false, error);
    return false;
  }
  if (http_status == 200) {
    // Newer directories return the domain in every heartbeat; a revoked code
    // shows up here immediately instead of waiting for a re-registration.
    try {
      const json response = json::parse(body);
      const std::string domain = response.value("domain", std::string());
      const bool valid = response.value("domain_valid", false);
      bool changed = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        changed = status_.domain != domain ||
                  status_.domain_valid != (valid && !domain.empty());
        status_.domain = domain;
        status_.domain_valid = valid && !domain.empty();
      }
      if (changed) save_credentials(active_url_);
    } catch (...) {
      // An empty body is a valid answer from older directories.
    }
    set_state(true, true, "");
    return true;
  }
  if (http_status == 204) {
    set_state(true, true, "");
    return true;
  }
  if (http_status == 401 || http_status == 404) {
    clear_credentials();
    set_state(false, false,
              "the server does not know this device; registering again");
    return false;
  }
  set_state(true, false,
            "server returned HTTP " + std::to_string(http_status));
  return false;
}

void server_client::run() {
  while (running_.load()) {
    const app_settings settings = auth_ ? auth_->settings() : app_settings{};
    const std::string custom_url = trim(settings.server_url);
    const std::string url = effective_server_url(settings);
    const std::string invite = trim(settings.server_invite);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      status_.url = url;
      status_.custom_url = custom_url;
      status_.default_url = default_server_url();
      status_.using_default = settings.server_enabled && custom_url.empty();
      status_.enabled = settings.server_enabled;
    }
    if (url.empty()) {
      active_url_.clear();
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.registered) {
          status_.registered = false;
          status_.online = false;
          status_.device_id.clear();
          status_.display_id.clear();
          status_.domain.clear();
          status_.domain_valid = false;
          token_.clear();
        }
      }
      for (int i = 0; i < 5 && running_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
      continue;
    }
    if (url != active_url_) {
      clear_credentials();
      active_url_ = url;
      failures_ = 0;
      last_attempt_ms_ = 0;
      load_credentials(url);
    }

    bool registered = false;
    uint64_t last_heartbeat = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      registered = status_.registered;
      last_heartbeat = status_.last_heartbeat_ms;
    }
    const uint64_t now = epoch_ms();
    if (!registered) {
      const uint64_t backoff = failures_ == 0
                                   ? 2000
                                   : (failures_ < 3 ? 5000 : 30000);
      if (now - last_attempt_ms_ >= backoff) {
        last_attempt_ms_ = now;
        if (try_register(url, invite)) {
          failures_ = 0;
        } else {
          ++failures_;
        }
      }
    } else if (now - last_heartbeat >= kHeartbeatMs &&
               now - last_attempt_ms_ >= 5000) {
      last_attempt_ms_ = now;
      if (try_heartbeat()) failures_ = 0;
    }
    if (reconfigure_.exchange(false)) {
      last_attempt_ms_ = 0;
      failures_ = 0;
    }
    for (int i = 0; i < 5 && running_.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  }
}

}  // namespace molink
