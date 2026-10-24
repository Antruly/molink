#include "config.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "logging.h"
#include "version.h"

namespace molink {
namespace {

using nlohmann::json;

std::string env_string(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string() : std::string(value);
}

bool parse_int_arg(const char* text, int& out) {
  if (text == nullptr || *text == '\0') return false;
  char* end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (end == nullptr || *end != '\0') return false;
  if (value < -2147483647L || value > 2147483647L) return false;
  out = static_cast<int>(value);
  return true;
}

bool json_port(const json& object, const char* key, int& out) {
  if (!object.contains(key)) return true;
  if (!object[key].is_number_integer()) return false;
  const long long value = object[key].get<long long>();
  if (value < 0 || value > 65535) return false;
  out = static_cast<int>(value);
  return true;
}

bool apply_json(const std::string& path, server_config& cfg, std::string& err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    err = "cannot open config file: " + path;
    return false;
  }
  json document;
  try {
    in >> document;
  } catch (const std::exception& e) {
    err = std::string("config file is not valid JSON: ") + e.what();
    return false;
  }
  if (!document.is_object()) {
    err = "config file must contain a JSON object";
    return false;
  }

  if (document.contains("server") && document["server"].is_object()) {
    const json& s = document["server"];
    cfg.data_dir = s.value("data_dir", cfg.data_dir);
    cfg.registration_token = s.value("registration_token", cfg.registration_token);
    cfg.public_url = s.value("public_url", cfg.public_url);
    cfg.offline_after_seconds =
        s.value("offline_after_seconds", cfg.offline_after_seconds);
  }
  if (document.contains("device") && document["device"].is_object()) {
    const json& d = document["device"];
    cfg.device.host = d.value("host", cfg.device.host);
    if (!json_port(d, "port", cfg.device.port)) {
      err = "device.port must be 0..65535";
      return false;
    }
    cfg.device.tls_cert = d.value("tls_cert", cfg.device.tls_cert);
    cfg.device.tls_key = d.value("tls_key", cfg.device.tls_key);
    cfg.device.web_root = d.value("web_root", cfg.device.web_root);
  }
  if (document.contains("admin") && document["admin"].is_object()) {
    const json& a = document["admin"];
    cfg.admin.host = a.value("host", cfg.admin.host);
    if (!json_port(a, "port", cfg.admin.port)) {
      err = "admin.port must be 0..65535";
      return false;
    }
    cfg.admin.tls_cert = a.value("tls_cert", cfg.admin.tls_cert);
    cfg.admin.tls_key = a.value("tls_key", cfg.admin.tls_key);
    cfg.admin.web_root = a.value("web_root", cfg.admin.web_root);
    cfg.allow_insecure_admin =
        a.value("allow_insecure_remote", cfg.allow_insecure_admin);
  }
  if (document.contains("database") && document["database"].is_object()) {
    const json& d = document["database"];
    cfg.database.driver = d.value("driver", cfg.database.driver);
    cfg.database.url = d.value("url", cfg.database.url);
    cfg.database.pool_min = d.value("pool_min", cfg.database.pool_min);
    cfg.database.pool_max = d.value("pool_max", cfg.database.pool_max);
    cfg.database.busy_timeout_ms =
        d.value("busy_timeout_ms", cfg.database.busy_timeout_ms);
  }
  if (document.contains("log") && document["log"].is_object()) {
    const json& l = document["log"];
    cfg.log.level = l.value("level", cfg.log.level);
    cfg.log.file = l.value("file", cfg.log.file);
    cfg.log.access_log = l.value("access_log", cfg.log.access_log);
    cfg.log.max_size_mb = l.value("max_size_mb", cfg.log.max_size_mb);
    cfg.log.max_files = l.value("max_files", cfg.log.max_files);
  }
  return true;
}

void apply_environment(server_config& cfg) {
  auto apply = [](const char* name, std::string& target) {
    const std::string value = env_string(name);
    if (!value.empty()) target = value;
  };
  auto apply_int = [](const char* name, int& target) {
    const std::string value = env_string(name);
    if (value.empty()) return;
    int parsed = 0;
    if (parse_int_arg(value.c_str(), parsed)) target = parsed;
  };

  apply("MOLINK_SERVER_DATA_DIR", cfg.data_dir);
  apply("MOLINK_SERVER_HOST", cfg.device.host);
  apply_int("MOLINK_SERVER_PORT", cfg.device.port);
  apply("MOLINK_SERVER_ADMIN_HOST", cfg.admin.host);
  apply_int("MOLINK_SERVER_ADMIN_PORT", cfg.admin.port);
  apply("MOLINK_SERVER_REGISTRATION_TOKEN", cfg.registration_token);
  apply("MOLINK_SERVER_PUBLIC_URL", cfg.public_url);
  apply("MOLINK_SERVER_LOG_LEVEL", cfg.log.level);
  apply("MOLINK_SERVER_LOG_FILE", cfg.log.file);
  apply("MOLINK_SERVER_UI_DIR", cfg.admin.web_root);
  apply("MOLINK_SERVER_SETUP_TOKEN", cfg.setup_token);
  apply("MOLINK_SERVER_DB_DRIVER", cfg.database.driver);
  apply("MOLINK_SERVER_DB_URL", cfg.database.url);
  apply_int("MOLINK_SERVER_DB_POOL_MIN", cfg.database.pool_min);
  apply_int("MOLINK_SERVER_DB_POOL_MAX", cfg.database.pool_max);
  apply_int("MOLINK_SERVER_DB_BUSY_TIMEOUT_MS", cfg.database.busy_timeout_ms);
  const std::string insecure = env_string("MOLINK_SERVER_ADMIN_ALLOW_INSECURE");
  if (insecure == "1" || insecure == "true") cfg.allow_insecure_admin = true;
}

bool parse_cli(int argc, char** argv, server_config& cfg, std::string& err) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](const char* name, std::string& target) -> bool {
      if (i + 1 >= argc) {
        err = std::string("missing value for ") + name;
        return false;
      }
      target = argv[++i];
      return true;
    };
    auto next_int = [&](const char* name, int& target) -> bool {
      if (i + 1 >= argc) {
        err = std::string("missing value for ") + name;
        return false;
      }
      if (!parse_int_arg(argv[++i], target)) {
        err = std::string("invalid value for ") + name;
        return false;
      }
      return true;
    };

    if (arg == "--config") {
      if (!next("--config", cfg.config_path)) return false;
    } else if (arg == "--help" || arg == "-h") {
      cfg.show_help = true;
    } else if (arg == "--version") {
      cfg.show_version = true;
    } else if (arg == "--host") {
      if (!next("--host", cfg.device.host)) return false;
    } else if (arg == "--port") {
      if (!next_int("--port", cfg.device.port)) return false;
    } else if (arg == "--tls-cert") {
      if (!next("--tls-cert", cfg.device.tls_cert)) return false;
    } else if (arg == "--tls-key") {
      if (!next("--tls-key", cfg.device.tls_key)) return false;
    } else if (arg == "--admin-host") {
      if (!next("--admin-host", cfg.admin.host)) return false;
    } else if (arg == "--admin-port") {
      if (!next_int("--admin-port", cfg.admin.port)) return false;
    } else if (arg == "--admin-tls-cert") {
      if (!next("--admin-tls-cert", cfg.admin.tls_cert)) return false;
    } else if (arg == "--admin-tls-key") {
      if (!next("--admin-tls-key", cfg.admin.tls_key)) return false;
    } else if (arg == "--ui-dir") {
      if (!next("--ui-dir", cfg.admin.web_root)) return false;
    } else if (arg == "--data-dir") {
      if (!next("--data-dir", cfg.data_dir)) return false;
    } else if (arg == "--registration-token") {
      if (!next("--registration-token", cfg.registration_token)) return false;
    } else if (arg == "--public-url") {
      if (!next("--public-url", cfg.public_url)) return false;
    } else if (arg == "--offline-after") {
      if (!next_int("--offline-after", cfg.offline_after_seconds)) return false;
    } else if (arg == "--db-driver") {
      if (!next("--db-driver", cfg.database.driver)) return false;
    } else if (arg == "--db-url") {
      if (!next("--db-url", cfg.database.url)) return false;
    } else if (arg == "--db-pool-min") {
      if (!next_int("--db-pool-min", cfg.database.pool_min)) return false;
    } else if (arg == "--db-pool-max") {
      if (!next_int("--db-pool-max", cfg.database.pool_max)) return false;
    } else if (arg == "--db-busy-timeout") {
      if (!next_int("--db-busy-timeout", cfg.database.busy_timeout_ms)) {
        return false;
      }
    } else if (arg == "--log-level") {
      if (!next("--log-level", cfg.log.level)) return false;
    } else if (arg == "--log-file") {
      if (!next("--log-file", cfg.log.file)) return false;
    } else if (arg == "--access-log") {
      cfg.log.access_log = true;
    } else if (arg == "--setup-token") {
      if (!next("--setup-token", cfg.setup_token)) return false;
    } else if (arg == "--allow-insecure-admin") {
      cfg.allow_insecure_admin = true;
    } else if (arg == "--skip-legacy-import") {
      cfg.skip_legacy_import = true;
    } else if (arg == "--print-ready") {
      cfg.print_ready = true;
    } else if (arg == "--install-service") {
      cfg.install_service = true;
    } else if (arg == "--uninstall-service") {
      cfg.uninstall_service = true;
    } else if (arg == "--service") {
      cfg.service_mode = true;
    } else if (arg == "--service-name") {
      if (!next("--service-name", cfg.service_name)) return false;
    } else {
      err = "unknown argument: " + arg;
      return false;
    }
  }
  return true;
}

bool validate(const server_config& cfg, std::string& err) {
  if (cfg.database.driver != "sqlite" && cfg.database.driver != "postgres") {
    err = "database.driver must be sqlite or postgres";
    return false;
  }
  if (cfg.database.pool_min < 1 || cfg.database.pool_min > 64) {
    err = "database.pool_min must be 1..64";
    return false;
  }
  if (cfg.database.pool_max < cfg.database.pool_min ||
      cfg.database.pool_max > 64) {
    err = "database.pool_max must be >= pool_min and <= 64";
    return false;
  }
  if (cfg.database.busy_timeout_ms < 0 ||
      cfg.database.busy_timeout_ms > 600000) {
    err = "database.busy_timeout_ms must be 0..600000";
    return false;
  }
  if (cfg.device.port < 0 || cfg.device.port > 65535 ||
      cfg.admin.port < 0 || cfg.admin.port > 65535) {
    err = "ports must be 0..65535";
    return false;
  }
  if (cfg.device.port != 0 && cfg.device.port == cfg.admin.port) {
    err = "device and admin ports must differ";
    return false;
  }
  if (cfg.device.tls_cert.empty() != cfg.device.tls_key.empty()) {
    err = "device tls_cert and tls_key must be set together";
    return false;
  }
  if (cfg.admin.tls_cert.empty() != cfg.admin.tls_key.empty()) {
    err = "admin tls_cert and tls_key must be set together";
    return false;
  }
  if (cfg.offline_after_seconds < 1 || cfg.offline_after_seconds > 86400) {
    err = "offline_after_seconds must be 1..86400";
    return false;
  }
  log_level level;
  if (!parse_log_level(cfg.log.level, level)) {
    err = "log.level must be debug, info, warn or error";
    return false;
  }
  if (cfg.log.max_size_mb < 0 || cfg.log.max_size_mb > 1024) {
    err = "log.max_size_mb must be 0..1024";
    return false;
  }
  if (cfg.log.max_files < 1 || cfg.log.max_files > 100) {
    err = "log.max_files must be 1..100";
    return false;
  }
  return true;
}

}  // namespace

std::string sqlite_url_for(const std::string& data_dir) {
  std::filesystem::path path(data_dir);
  path /= "molink.db";
  std::error_code ec;
  const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
  const std::string file = ec ? path.string() : absolute.string();
  // The libuvcpp form is sqlite:///<absolute path>.
  std::string normalized = file;
  for (char& c : normalized) {
    if (c == '\\') c = '/';
  }
#if defined(_WIN32)
  // libuvcpp hands everything after `sqlite://` to SQLite as the path. On
  // Windows that must be `C:/...`, not `/C:/...`, so use two slashes.
  return "sqlite://" + normalized;
#else
  if (!normalized.empty() && normalized.front() != '/') {
    normalized.insert(normalized.begin(), '/');
  }
  return "sqlite:///" + normalized;
#endif
}

bool parse_config(int argc, char** argv, server_config& out, std::string& err) {
  server_config cfg;
  cfg.admin.host = "127.0.0.1";
  cfg.admin.port = 28431;

  // The config path is needed before the file can be loaded, so scan for it
  // once and let CLI win over the environment.
  std::string config_path = env_string("MOLINK_SERVER_CONFIG");
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--config") == 0) config_path = argv[i + 1];
  }
  if (!config_path.empty()) {
    cfg.config_path = config_path;
    if (!apply_json(config_path, cfg, err)) return false;
  }

  apply_environment(cfg);
  if (!parse_cli(argc, argv, cfg, err)) return false;
  if (cfg.config_path.empty()) cfg.config_path = env_string("MOLINK_SERVER_CONFIG");
  if (!validate(cfg, err)) return false;
  if (cfg.database.url.empty()) {
    if (cfg.database.driver != "sqlite") {
      err = "database.url is required for postgres";
      return false;
    }
    cfg.database.url = sqlite_url_for(cfg.data_dir);
  }
  out = std::move(cfg);
  return true;
}

}  // namespace molink
