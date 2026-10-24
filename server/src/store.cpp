#include "store.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

namespace molink_server {
namespace {

using nlohmann::json;

constexpr char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

uint64_t epoch_ms() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch())
          .count());
}

std::string sanitize_name(std::string value) {
  std::string out;
  out.reserve(value.size());
  for (unsigned char c : value) {
    if (c < 0x20 || c == 0x7f) continue;
    out.push_back(static_cast<char>(c));
  }
  while (!out.empty() && out.front() == ' ') out.erase(out.begin());
  while (!out.empty() && out.back() == ' ') out.pop_back();
  if (out.empty()) out = "device";
  if (out.size() > 96) {
    std::size_t cut = 96;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xc0) == 0x80) {
      --cut;
    }
    out.resize(cut);
  }
  return out;
}

std::string json_to_string(const json& value) {
  return value.is_string() ? value.get<std::string>() : std::string();
}

}  // namespace

device_store::device_store(std::string path, int offline_after_seconds)
    : path_(std::move(path)),
      offline_after_seconds_(offline_after_seconds > 0 ? offline_after_seconds
                                                       : 90) {}

bool device_store::load(std::string& err) {
  devices_.clear();
  std::ifstream in(path_, std::ios::binary);
  if (!in) return true;  // First run: no file yet.
  std::ostringstream buffer;
  buffer << in.rdbuf();
  json document;
  try {
    document = json::parse(buffer.str());
  } catch (const std::exception& error) {
    err = std::string("device file is not valid JSON: ") + error.what();
    return false;
  }
  if (!document.is_object() || !document.contains("devices") ||
      !document["devices"].is_array()) {
    err = "device file has no devices array";
    return false;
  }
  for (const json& item : document["devices"]) {
    if (!item.is_object()) continue;
    device_record record;
    record.id = normalize_id(json_to_string(item.value("id", "")));
    if (record.id.empty()) continue;
    record.token = json_to_string(item.value("token", ""));
    record.name = json_to_string(item.value("name", ""));
    record.address = json_to_string(item.value("address", ""));
    record.peer_ip = json_to_string(item.value("peer_ip", ""));
    record.port = item.value("port", 0);
    record.version = json_to_string(item.value("version", ""));
    record.created_ms = item.value("created_ms", 0ull);
    record.last_seen_ms = item.value("last_seen_ms", 0ull);
    devices_.emplace(record.id, std::move(record));
  }
  return true;
}

bool device_store::save(std::string& err) const {
  json list = json::array();
  for (const auto& pair : devices_) {
    const device_record& record = pair.second;
    json item;
    item["id"] = record.id;
    item["token"] = record.token;
    item["name"] = record.name;
    item["address"] = record.address;
    item["peer_ip"] = record.peer_ip;
    item["port"] = record.port;
    item["version"] = record.version;
    item["created_ms"] = record.created_ms;
    item["last_seen_ms"] = record.last_seen_ms;
    list.push_back(std::move(item));
  }
  json document;
  document["devices"] = std::move(list);
  const std::string temporary = path_ + ".tmp";
  {
    std::ofstream out(temporary,
                      std::ios::binary | std::ios::trunc);
    if (!out) {
      err = "could not open the device file for writing";
      return false;
    }
    out << document.dump(2) << '\n';
    if (!out) {
      err = "could not write the device file";
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(temporary, path_, ec);
  if (ec) {
    err = "could not replace the device file: " + ec.message();
    return false;
  }
  dirty_ = false;
  last_save_ms_ = epoch_ms();
  return true;
}

std::string device_store::normalize_id(const std::string& raw) {
  std::string out;
  out.reserve(raw.size());
  for (unsigned char c : raw) {
    if (c == '-' || c == ' ' || c == '_') continue;
    if (!std::isalnum(c)) continue;
    out.push_back(static_cast<char>(std::toupper(c)));
  }
  return out;
}

std::string device_store::display_id(const std::string& raw) {
  const std::string normalized = normalize_id(raw);
  if (normalized.size() != 9) return normalized;
  return normalized.substr(0, 3) + "-" + normalized.substr(3, 3) + "-" +
         normalized.substr(6, 3);
}

std::string device_store::generate_id() const {
  std::random_device device;
  std::mt19937 generator(device());
  std::uniform_int_distribution<std::size_t> pick(
      0, sizeof(kAlphabet) - 2);
  for (int attempt = 0; attempt < 100; ++attempt) {
    std::string id(9, 'A');
    for (char& c : id) c = kAlphabet[pick(generator)];
    if (devices_.find(id) == devices_.end()) return id;
  }
  return std::string();
}

std::string device_store::generate_token() {
  std::random_device device;
  std::mt19937_64 generator(device());
  std::uniform_int_distribution<int> nibble(0, 15);
  static const char hex[] = "0123456789abcdef";
  std::string token(32, '0');
  for (char& c : token) c = hex[nibble(generator)];
  return token;
}

bool device_store::register_device(const std::string& name,
                                   const std::string& address,
                                   const std::string& peer_ip, int port,
                                   const std::string& version,
                                   device_record& out, std::string& err) {
  if (port <= 0 || port > 65535) {
    err = "port must be between 1 and 65535";
    return false;
  }
  device_record record;
  record.id = generate_id();
  if (record.id.empty()) {
    err = "could not allocate a device id";
    return false;
  }
  record.token = generate_token();
  record.name = sanitize_name(name);
  record.address = address;
  record.peer_ip = peer_ip;
  record.port = port;
  record.version = version;
  record.created_ms = epoch_ms();
  record.last_seen_ms = record.created_ms;
  out = record;
  devices_.emplace(record.id, std::move(record));
  dirty_ = true;
  return save(err);
}

bool device_store::heartbeat(const std::string& id, const std::string& token,
                             const std::string& address,
                             const std::string& peer_ip, int port,
                             const std::string& version, std::string& err) {
  const std::string normalized = normalize_id(id);
  auto it = devices_.find(normalized);
  if (it == devices_.end()) {
    err = "unknown device";
    return false;
  }
  if (it->second.token.empty() || it->second.token != token) {
    err = "invalid device token";
    return false;
  }
  device_record& record = it->second;
  record.last_seen_ms = epoch_ms();
  if (!address.empty()) record.address = address;
  if (!peer_ip.empty()) record.peer_ip = peer_ip;
  if (port > 0 && port <= 65535) record.port = port;
  if (!version.empty()) record.version = version;
  dirty_ = true;
  flush_if_due();
  return true;
}

bool device_store::unregister_device(const std::string& id,
                                     const std::string& token) {
  const std::string normalized = normalize_id(id);
  auto it = devices_.find(normalized);
  if (it == devices_.end()) return false;
  if (it->second.token.empty() || it->second.token != token) return false;
  devices_.erase(it);
  dirty_ = true;
  std::string error;
  save(error);
  return true;
}

bool device_store::find(const std::string& id, device_record& out) const {
  const std::string normalized = normalize_id(id);
  auto it = devices_.find(normalized);
  if (it == devices_.end()) return false;
  out = it->second;
  return true;
}

bool device_store::online(const device_record& record) const {
  if (record.last_seen_ms == 0) return false;
  const uint64_t now = epoch_ms();
  return now <= record.last_seen_ms +
                    static_cast<uint64_t>(offline_after_seconds_) * 1000;
}

std::size_t device_store::online_count() const {
  std::size_t count = 0;
  for (const auto& pair : devices_) {
    if (online(pair.second)) ++count;
  }
  return count;
}

void device_store::flush_if_due(uint64_t interval_ms) {
  if (!dirty_) return;
  const uint64_t now = epoch_ms();
  if (now - last_save_ms_ < interval_ms) return;
  std::string error;
  save(error);
}

}  // namespace molink_server
