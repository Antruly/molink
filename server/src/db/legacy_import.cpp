#include "db/legacy_import.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

#include "logging.h"
#include "util.h"

namespace molink {
namespace {

std::string normalize_id(const std::string& raw) {
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

std::string search_key(const std::string& name, const std::string& id,
                       const std::string& peer_ip) {
  return lowercase_ascii(name + " " + id + " " + peer_ip);
}

}  // namespace

bool import_legacy_devices(database& db, uvcpp::uvcpp_db_client& client,
                           const std::string& data_dir, std::string& error) {
  const std::filesystem::path path =
      std::filesystem::path(data_dir) / "devices.json";
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return true;

  uvcpp::uvcpp_db_table table;
  const auto count_status = client.query("SELECT COUNT(*) AS n FROM devices", &table);
  if (!uvcpp::uvcpp_db_ok(count_status)) {
    error = "legacy import: " + client.last_error();
    return false;
  }
  if (table.value("n").to_int64() != 0) {
    log_info("legacy devices.json kept: the database already has devices");
    return true;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "legacy import: cannot read " + path.string();
    return false;
  }
  nlohmann::json document;
  try {
    input >> document;
  } catch (const std::exception& e) {
    error = std::string("legacy import: devices.json is not valid JSON: ") +
            e.what();
    return false;
  }
  if (!document.is_object() || !document.contains("devices") ||
      !document["devices"].is_array()) {
    error = "legacy import: devices.json has no devices array";
    return false;
  }

  const sql_dialect& dialect = db.dialect();
  const std::string insert =
      "INSERT INTO devices (id, token_hash, name, address, peer_ip, port, "
      "version, search_key, created_ms, last_seen_ms) VALUES (" +
      dialect.placeholder(1) + ", " + dialect.placeholder(2) + ", " +
      dialect.placeholder(3) + ", " + dialect.placeholder(4) + ", " +
      dialect.placeholder(5) + ", " + dialect.placeholder(6) + ", " +
      dialect.placeholder(7) + ", " + dialect.placeholder(8) + ", " +
      dialect.placeholder(9) + ", " + dialect.placeholder(10) +
      ") ON CONFLICT (id) DO NOTHING";

  if (!uvcpp::uvcpp_db_ok(client.begin())) {
    error = "legacy import: " + client.last_error();
    return false;
  }

  std::size_t imported = 0;
  for (const nlohmann::json& item : document["devices"]) {
    if (!item.is_object()) continue;
    std::string id;
    if (item.contains("id") && item["id"].is_string()) {
      id = normalize_id(item["id"].get<std::string>());
    }
    if (id.empty()) continue;
    std::string name = "device";
    if (item.contains("name") && item["name"].is_string()) {
      name = item["name"].get<std::string>();
      if (name.size() > 96) name.resize(96);
    }
    std::string token;
    if (item.contains("token") && item["token"].is_string()) {
      token = item["token"].get<std::string>();
    }
    const std::string address =
        item.value("address", std::string());
    const std::string peer_ip =
        item.value("peer_ip", std::string());
    int port = 0;
    if (item.contains("port") && item["port"].is_number_integer()) {
      port = item["port"].get<int>();
      if (port < 0 || port > 65535) port = 0;
    }
    const std::string version = item.value("version", std::string());
    const std::uint64_t created =
        item.value("created_ms", static_cast<std::uint64_t>(epoch_ms()));
    const std::uint64_t seen =
        item.value("last_seen_ms", static_cast<std::uint64_t>(created));
    const uvcpp::uvcpp_db_params params{
        id, sha256_hex(token), name, address, peer_ip, port, version,
        search_key(name, id, peer_ip), static_cast<int64_t>(created),
        static_cast<int64_t>(seen)};
    int64_t affected = 0;
    const auto status = client.execute(insert, params, &affected);
    if (!uvcpp::uvcpp_db_ok(status)) {
      client.rollback();
      error = "legacy import: " + client.last_error();
      return false;
    }
    if (affected > 0) ++imported;
  }

  if (!uvcpp::uvcpp_db_ok(client.commit())) {
    client.rollback();
    error = "legacy import: could not commit: " + client.last_error();
    return false;
  }

  const std::string renamed =
      path.string() + ".imported." + std::to_string(epoch_ms());
  std::error_code rename_error;
  std::filesystem::rename(path, renamed, rename_error);
  if (rename_error) {
    log_warn("legacy import: imported %zu devices but could not rename %s: %s",
             imported, path.string().c_str(), rename_error.message().c_str());
  } else {
    log_info("legacy import: imported %zu devices from %s", imported,
             renamed.c_str());
  }
  return true;
}

}  // namespace molink
