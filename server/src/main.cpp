// moLink Server — phase 2 device directory.
//
// Devices register once, keep the connection alive with a heartbeat, and any
// client can look up a device ID to obtain the address and port for a direct
// session. Media never passes through this service; a relay/TURN layer can be
// added later without changing the registration API.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_json.h>

#include "store.h"

namespace {

using uvcpp::uvcpp_json;
using uvcpp::uvcpp_web_next;
using uvcpp::uvcpp_web_request;
using uvcpp::uvcpp_web_response;

constexpr char kService[] = "moLink Server";
constexpr char kVersion[] = "0.1.0";

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop.store(true); }

uint64_t epoch_ms() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch())
          .count());
}

struct config {
  std::string host = "0.0.0.0";
  int port = 28430;
  std::string data_dir = "./molink-server-data";
  std::string registration_token;
  std::string public_url;
  int offline_after = 90;
  bool show_help = false;
  bool show_version = false;
};

void print_usage() {
  std::printf(
      "molink-server %s\n\n"
      "  --host <addr>                bind address (default 0.0.0.0)\n"
      "  --port <n>                   HTTP port (default 28430)\n"
      "  --data-dir <path>            device directory storage "
      "(default ./molink-server-data)\n"
      "  --registration-token <t>     require this invite value to register\n"
      "  --public-url <url>           URL advertised to clients\n"
      "  --offline-after <seconds>    presence timeout (default 90)\n"
      "  --version                    print version and exit\n"
      "  --help                       print this help\n",
      kVersion);
}

bool parse_args(int argc, char** argv, config& out) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&](std::string& target) -> bool {
      if (i + 1 >= argc) return false;
      target = argv[++i];
      return true;
    };
    if (arg == "--host") {
      if (!value(out.host)) return false;
    } else if (arg == "--port") {
      std::string text;
      if (!value(text)) return false;
      out.port = std::atoi(text.c_str());
    } else if (arg == "--data-dir") {
      if (!value(out.data_dir)) return false;
    } else if (arg == "--registration-token") {
      if (!value(out.registration_token)) return false;
    } else if (arg == "--public-url") {
      if (!value(out.public_url)) return false;
    } else if (arg == "--offline-after") {
      std::string text;
      if (!value(text)) return false;
      out.offline_after = std::atoi(text.c_str());
    } else if (arg == "--version") {
      out.show_version = true;
    } else if (arg == "--help" || arg == "-h") {
      out.show_help = true;
    } else {
      std::fprintf(stderr, "molink-server: unknown option %s\n", arg.c_str());
      return false;
    }
  }
  if (out.port <= 0 || out.port > 65535) {
    std::fprintf(stderr, "molink-server: invalid port\n");
    return false;
  }
  return true;
}

std::string query_value(uvcpp_web_request& req, const char* key) {
  const std::string* value = req.query(key);
  return value != nullptr ? *value : std::string();
}

int query_int(uvcpp_web_request& req, const char* key, int fallback) {
  const std::string value = query_value(req, key);
  if (value.empty()) return fallback;
  try {
    return std::stoi(value);
  } catch (...) {
    return fallback;
  }
}

void send_error(uvcpp_web_response& resp, int status, const char* code,
                const std::string& message) {
  uvcpp_json body;
  body["type"] = "error";
  body["code"] = code;
  body["message"] = message;
  resp.status(status).json(body);
  resp.end();
}

std::string effective_address(const molink_server::device_record& record) {
  return record.address.empty() ? record.peer_ip : record.address;
}

// Registration is open by default for a self-hosted server, but capped per
// source address so one client cannot fill the directory.
std::unordered_map<std::string, std::pair<uint64_t, int>> g_registrations;

bool allow_registration(const std::string& peer_ip) {
  const uint64_t now = epoch_ms();
  auto& entry = g_registrations[peer_ip];
  if (now - entry.first > 60ull * 60 * 1000) {
    entry.first = now;
    entry.second = 0;
  }
  if (entry.second >= 20) return false;
  ++entry.second;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  config cfg;
  if (!parse_args(argc, argv, cfg)) return 2;
  if (cfg.show_help) {
    print_usage();
    return 0;
  }
  if (cfg.show_version) {
    std::printf("molink-server %s\n", kVersion);
    return 0;
  }

  std::error_code ec;
  std::filesystem::create_directories(cfg.data_dir, ec);
  if (ec) {
    std::fprintf(stderr, "molink-server: cannot create %s: %s\n",
                 cfg.data_dir.c_str(), ec.message().c_str());
    return 1;
  }
  const std::string device_file =
      (std::filesystem::path(cfg.data_dir) / "devices.json").string();
  molink_server::device_store store(device_file, cfg.offline_after);
  std::string error;
  if (!store.load(error)) {
    std::fprintf(stderr, "molink-server: %s\n", error.c_str());
    return 1;
  }

  uvcpp::uvcpp_web_app app;
  app.set_host(cfg.host)
      .set_port(cfg.port)
      .set_server_header(std::string(kService) + "/" + kVersion)
      .set_access_log(false)
      .set_max_body_size(64u * 1024u);

  app.use([](uvcpp_web_request& req, uvcpp_web_response& resp,
             uvcpp_web_next next) {
    resp.set_header("Access-Control-Allow-Origin", "*");
    resp.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    resp.set_header("Access-Control-Allow-Headers", "Content-Type");
    resp.set_header("X-Content-Type-Options", "nosniff");
    if (req.method() == uvcpp::http_method::HTTP_OPTIONS) {
      resp.status(204).end();
      return;
    }
    next();
  });

  app.get("/", [&cfg, &store](uvcpp_web_request&, uvcpp_web_response& resp,
                              uvcpp_web_next) {
    uvcpp_json j;
    j["service"] = kService;
    j["version"] = kVersion;
    j["devices"] = store.size();
    j["online"] = store.online_count();
    j["public_url"] = cfg.public_url;
    j["endpoints"]["register"] = "POST /v1/devices/register";
    j["endpoints"]["heartbeat"] = "POST /v1/devices/heartbeat";
    j["endpoints"]["lookup"] = "GET /v1/devices?id=ABC-DEF-GHJ";
    j["endpoints"]["unregister"] = "POST /v1/devices/unregister";
    resp.set_header("Cache-Control", "no-store").json(j);
    resp.end();
  });

  auto health = [&store](uvcpp_web_request&, uvcpp_web_response& resp,
                         uvcpp_web_next) {
    uvcpp_json j;
    j["status"] = "ok";
    j["service"] = kService;
    j["version"] = kVersion;
    j["devices"] = store.size();
    j["online"] = store.online_count();
    resp.set_header("Cache-Control", "no-store").json(j);
    resp.end();
  };
  app.get("/healthz", health);
  app.get("/api/health", health);

  app.post("/v1/devices/register",
           [&cfg, &store](uvcpp_web_request& req, uvcpp_web_response& resp,
                          uvcpp_web_next) {
             const std::string peer_ip = req.peer_ip();
             if (!cfg.registration_token.empty() &&
                 query_value(req, "invite") != cfg.registration_token) {
               send_error(resp, 403, "invite_required",
                          "this server requires a registration invite");
               return;
             }
             if (!allow_registration(peer_ip)) {
               send_error(resp, 429, "too_many_registrations",
                          "too many devices registered from this address");
               return;
             }
             const int port = query_int(req, "port", 0);
             molink_server::device_record record;
             std::string error;
             if (!store.register_device(query_value(req, "name"),
                                        query_value(req, "address"), peer_ip,
                                        port, query_value(req, "version"),
                                        record, error)) {
               send_error(resp, 400, "registration_failed", error);
               return;
             }
             uvcpp_json j;
             j["ok"] = true;
             j["device_id"] = record.id;
             j["display_id"] = molink_server::device_store::display_id(record.id);
             j["token"] = record.token;
             j["server_time"] = epoch_ms();
             j["public_url"] = cfg.public_url;
             resp.set_header("Cache-Control", "no-store").json(j);
             resp.end();
             std::fprintf(stderr,
                          "molink-server: registered %s (%s:%d) from %s\n",
                          record.name.c_str(), record.id.c_str(), record.port,
                          peer_ip.c_str());
           });

  app.post("/v1/devices/heartbeat",
           [&store](uvcpp_web_request& req, uvcpp_web_response& resp,
                    uvcpp_web_next) {
             const std::string id = query_value(req, "id");
             const std::string token = query_value(req, "token");
             if (id.empty() || token.empty()) {
               send_error(resp, 400, "missing_fields",
                          "id and token are required");
               return;
             }
             std::string error;
             if (!store.heartbeat(id, token, query_value(req, "address"),
                                  req.peer_ip(), query_int(req, "port", 0),
                                  query_value(req, "version"), error)) {
               send_error(resp, 401, "heartbeat_rejected", error);
               return;
             }
             resp.status(204).end();
           });

  auto lookup = [&cfg, &store](uvcpp_web_request& req, uvcpp_web_response& resp,
                               uvcpp_web_next) {
    const std::string id = query_value(req, "id");
    if (id.empty()) {
      send_error(resp, 400, "missing_id", "id is required");
      return;
    }
    molink_server::device_record record;
    if (!store.find(id, record)) {
      send_error(resp, 404, "unknown_device",
                 "no device is registered with that id");
      return;
    }
    const bool online = store.online(record);
    uvcpp_json j;
    j["id"] = record.id;
    j["display_id"] = molink_server::device_store::display_id(record.id);
    j["name"] = record.name;
    j["online"] = online;
    j["address"] = effective_address(record);
    j["peer_ip"] = record.peer_ip;
    j["port"] = record.port;
    j["version"] = record.version;
    j["last_seen_ms"] = record.last_seen_ms;
    j["public_url"] = cfg.public_url;
    resp.set_header("Cache-Control", "no-store").json(j);
    resp.end();
  };
  app.get("/v1/devices", lookup);
  app.get("/v1/devices/lookup", lookup);

  app.post("/v1/devices/unregister",
           [&store](uvcpp_web_request& req, uvcpp_web_response& resp,
                    uvcpp_web_next) {
             const std::string id = query_value(req, "id");
             const std::string token = query_value(req, "token");
             if (id.empty() || token.empty()) {
               send_error(resp, 400, "missing_fields",
                          "id and token are required");
               return;
             }
             if (!store.unregister_device(id, token)) {
               send_error(resp, 401, "unregister_rejected",
                          "unknown device or invalid token");
               return;
             }
             resp.status(204).end();
           });

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  const int rc = app.start();
  if (rc != 0) {
    std::fprintf(stderr, "molink-server: cannot listen on %s:%d (libuv %d)\n",
                 cfg.host.c_str(), cfg.port, rc);
    return 1;
  }
  const int bound = app.bound_port() > 0 ? app.bound_port() : cfg.port;
  std::fprintf(stderr,
               "moLink Server %s listening on http://%s:%d\n"
               "  devices: %zu (%zu online)\n"
               "  data:    %s\n"
               "  invite:  %s\n",
               kVersion, cfg.host.c_str(), bound, store.size(),
               store.online_count(), device_file.c_str(),
               cfg.registration_token.empty() ? "(open)"
                                              : cfg.registration_token.c_str());

  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    store.flush_if_due(5000);
  }

  std::string save_error;
  store.save(save_error);
  app.stop();
  app.join();
  return 0;
}
