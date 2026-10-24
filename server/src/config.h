#pragma once

#include <string>

namespace molink {

struct listener_config {
  std::string host = "0.0.0.0";
  int port = 28430;
  std::string tls_cert;
  std::string tls_key;
  std::string web_root;
};

struct database_config {
  std::string driver = "sqlite";
  std::string url;
  int pool_min = 1;
  int pool_max = 8;
  int busy_timeout_ms = 5000;
};

struct log_config {
  std::string level = "info";
  std::string file;
  bool access_log = false;
  int max_size_mb = 20;
  int max_files = 5;
};

struct server_config {
  std::string config_path;
  std::string data_dir = "./molink-server-data";
  listener_config device;
  listener_config admin;
  database_config database;
  log_config log;

  std::string registration_token;
  std::string public_url;
  int offline_after_seconds = 90;

  std::string setup_token;
  bool allow_insecure_admin = false;
  bool skip_legacy_import = false;
  bool print_ready = false;

  // Windows service mode.
  std::string service_name = "moLinkServer";
  bool install_service = false;
  bool uninstall_service = false;
  bool service_mode = false;

  bool show_help = false;
  bool show_version = false;
};

/// Precedence: defaults < config file < environment < CLI.
bool parse_config(int argc, char** argv, server_config& out, std::string& err);

/// The effective SQLite URL for a data directory.
std::string sqlite_url_for(const std::string& data_dir);

}  // namespace molink
