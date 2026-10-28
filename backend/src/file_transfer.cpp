#include "file_transfer.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

#include <webapp/uvcpp_web_json.h>

#include "util.h"

namespace molink {
namespace {

namespace fs = std::filesystem;
using uvcpp::uvcpp_json;

std::string dump_json(const uvcpp_json& value) {
  return value.dump(-1, ' ',
                    /*ensure_ascii=*/false,
                    nlohmann::json::error_handler_t::replace);
}

std::string home_directory() {
#if defined(_WIN32)
  const char* profile = std::getenv("USERPROFILE");
  if (profile != nullptr && *profile != '\0') return profile;
  const char* drive = std::getenv("HOMEDRIVE");
  const char* path = std::getenv("HOMEPATH");
  if (drive != nullptr && path != nullptr) return std::string(drive) + path;
  return "C:\\";
#else
  const char* home = std::getenv("HOME");
  return home != nullptr && *home != '\0' ? home : "/";
#endif
}

std::string lowered(std::string value) {
#if defined(_WIN32)
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
  return value;
}

bool within_root(const fs::path& root, const fs::path& candidate) {
  auto r = root.begin();
  auto c = candidate.begin();
  for (; r != root.end(); ++r, ++c) {
    if (c == candidate.end()) return false;
    if (lowered(r->string()) != lowered(c->string())) return false;
  }
  return true;
}

fs::path home_root() {
  std::error_code ec;
  fs::path root = fs::weakly_canonical(fs::path(home_directory()), ec);
  return ec ? fs::path(home_directory()) : root;
}

/// Resolve a peer-supplied path. Relative paths are relative to home; an empty
/// path is home itself. Anything that escapes home is rejected, including
/// through a symlink.
std::optional<fs::path> resolve_path(const std::string& raw) {
  const fs::path root = home_root();
  fs::path requested = raw.empty() ? root : fs::path(raw);
  if (requested.is_relative()) requested = root / requested;
  std::error_code ec;
  const fs::path canonical = fs::weakly_canonical(requested, ec);
  if (ec || !within_root(root, canonical)) return std::nullopt;
  return canonical;
}

std::string safe_file_name(const std::string& raw) {
  fs::path parsed(raw);
  std::string name = parsed.filename().string();
  std::string out;
  out.reserve(name.size());
  for (unsigned char c : name) {
    if (c < 0x20 || c == 0x7f) continue;
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
        c == '"' || c == '<' || c == '>' || c == '|') {
      continue;
    }
    out.push_back(static_cast<char>(c));
  }
  while (!out.empty() && (out.front() == ' ' || out.front() == '.')) {
    out.erase(out.begin());
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
    out.pop_back();
  }
  if (out.empty()) out = "file";
  if (out.size() > 180) {
    std::size_t cut = 180;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xc0) == 0x80) {
      --cut;
    }
    out.resize(cut);
  }
  return out;
}

std::string unique_path_in(const fs::path& dir, const std::string& name) {
  fs::path candidate = dir / name;
  std::error_code ec;
  if (!fs::exists(candidate, ec)) return candidate.string();
  const fs::path stem = fs::path(name).stem();
  const std::string extension = fs::path(name).extension().string();
  for (int i = 1; i < 10000; ++i) {
    candidate = dir / (stem.string() + " (" + std::to_string(i) + ")" +
                       extension);
    if (!fs::exists(candidate, ec)) return candidate.string();
  }
  return (dir / (stem.string() + "-" + std::to_string(now_ms()) + extension))
      .string();
}

uint64_t file_mtime_ms(const fs::path& path) {
  std::error_code ec;
  const auto time = fs::last_write_time(path, ec);
  if (ec) return 0;
  const auto delta = time - decltype(time)::clock::now();
  const auto system =
      std::chrono::system_clock::now() +
      std::chrono::duration_cast<std::chrono::system_clock::duration>(delta);
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          system.time_since_epoch())
          .count());
}

std::string receive_directory() {
  const fs::path dir = home_root() / "Downloads" / "moLink";
  std::error_code ec;
  fs::create_directories(dir, ec);
  return dir.string();
}

}  // namespace

file_transfer::file_transfer(json_sender send_json, chunk_sender send_chunk)
    : send_json_(std::move(send_json)), send_chunk_(std::move(send_chunk)) {}

file_transfer::~file_transfer() { close_all(); }

void file_transfer::fail(const std::string& action, uint32_t id,
                         const std::string& why) {
  uvcpp_json j;
  j["type"] = "file";
  j["action"] = action;
  if (id != 0) j["id"] = id;
  j["ok"] = false;
  j["message"] = why;
  send_json_(dump_json(j));
}

void file_transfer::send_simple(const std::string& action, uint32_t id) {
  uvcpp_json j;
  j["type"] = "file";
  j["action"] = action;
  if (id != 0) j["id"] = id;
  j["ok"] = true;
  send_json_(dump_json(j));
}

void file_transfer::offer(const std::string& name, uint64_t size) {
  if (size > kMaxFileBytes) {
    fail("reject", 0, "file is larger than 8 GiB");
    return;
  }
  const std::string dir = receive_directory();
  if (dir.empty()) {
    fail("reject", 0, "could not create the receive folder");
    return;
  }
  const uint32_t id = next_id_++;
  upload item;
  item.id = id;
  item.name = safe_file_name(name);
  item.final_dir = dir;
  item.temp_path = (fs::path(dir) / (".upload-" + std::to_string(id) + ".part"))
                       .string();
  item.size = size;
  item.out.open(item.temp_path, std::ios::binary | std::ios::trunc);
  if (!item.out) {
    fail("reject", 0, "could not create the file");
    return;
  }
  uploads_.emplace(id, std::move(item));
  uvcpp_json j;
  j["type"] = "file";
  j["action"] = "accept";
  j["ok"] = true;
  j["id"] = id;
  j["chunk"] = kChunkBytes;
  j["dir"] = dir;
  send_json_(dump_json(j));
}

void file_transfer::chunk(uint32_t id, uint64_t offset, const uint8_t* data,
                          std::size_t len) {
  auto it = uploads_.find(id);
  if (it == uploads_.end()) return;
  upload& item = it->second;
  if (offset != item.received || item.received + len > item.size) {
    fail("cancel", id, "file chunk is out of order");
    drop_upload(id, true);
    return;
  }
  item.out.write(reinterpret_cast<const char*>(data),
                 static_cast<std::streamsize>(len));
  if (!item.out) {
    fail("cancel", id, "could not write the file");
    drop_upload(id, true);
    return;
  }
  item.received += len;
}

void file_transfer::complete(uint32_t id) {
  auto it = uploads_.find(id);
  if (it == uploads_.end()) {
    fail("done", id, "unknown transfer");
    return;
  }
  upload& item = it->second;
  if (item.received != item.size) {
    fail("done", id, "file ended early");
    drop_upload(id, true);
    return;
  }
  item.out.close();
  const std::string final_path =
      unique_path_in(fs::path(item.final_dir), item.name);
  std::error_code ec;
  fs::rename(item.temp_path, final_path, ec);
  if (ec) {
    fail("done", id, "could not finish the file");
    drop_upload(id, true);
    return;
  }
  uploads_.erase(it);
  uvcpp_json j;
  j["type"] = "file";
  j["action"] = "done";
  j["ok"] = true;
  j["id"] = id;
  j["path"] = final_path;
  send_json_(dump_json(j));
}

void file_transfer::cancel(uint32_t id) {
  if (uploads_.find(id) == uploads_.end() &&
      downloads_.find(id) == downloads_.end()) {
    return;
  }
  drop_upload(id, true);
  downloads_.erase(id);
  send_simple("cancelled", id);
}

void file_transfer::browse(const std::string& path) {
  const std::optional<fs::path> resolved = resolve_path(path);
  if (!resolved) {
    fail("listing", 0, "path is outside the home folder");
    return;
  }
  std::error_code ec;
  if (!fs::is_directory(*resolved, ec)) {
    fail("listing", 0, "not a folder");
    return;
  }
  const fs::path root = home_root();
  std::vector<uvcpp_json> entries;
  std::size_t count = 0;
  fs::directory_iterator it(*resolved,
                            fs::directory_options::skip_permission_denied,
                            ec);
  if (ec) {
    fail("listing", 0, "could not read the folder");
    return;
  }
  for (const fs::directory_entry& entry : it) {
    if (count >= 2000) break;
    std::error_code entry_ec;
    const fs::path canonical = fs::weakly_canonical(entry.path(), entry_ec);
    if (entry_ec || !within_root(root, canonical)) continue;
    const bool dir = entry.is_directory(entry_ec);
    if (entry_ec) continue;
    uvcpp_json item;
    item["name"] = entry.path().filename().string();
    item["dir"] = dir;
    item["size"] = dir ? 0 : static_cast<uint64_t>(entry.file_size(entry_ec));
    item["mtime"] = file_mtime_ms(entry.path());
    entries.push_back(std::move(item));
    ++count;
  }
  std::sort(entries.begin(), entries.end(),
            [](const uvcpp_json& a, const uvcpp_json& b) {
              const bool ad = a.value("dir", false);
              const bool bd = b.value("dir", false);
              if (ad != bd) return ad;
              return lowered(a.value("name", std::string())) <
                     lowered(b.value("name", std::string()));
            });
  uvcpp_json list = uvcpp_json::array();
  for (uvcpp_json& item : entries) list.push_back(std::move(item));
  uvcpp_json j;
  j["type"] = "file";
  j["action"] = "listing";
  j["ok"] = true;
  j["path"] = resolved->string();
  j["root"] = root.string();
  j["entries"] = std::move(list);
  send_json_(dump_json(j));
}

void file_transfer::request(const std::string& path) {
  const std::optional<fs::path> resolved = resolve_path(path);
  if (!resolved) {
    fail("sending", 0, "path is outside the home folder");
    return;
  }
  std::error_code ec;
  if (!fs::is_regular_file(*resolved, ec)) {
    fail("sending", 0, "not a file");
    return;
  }
  const uint64_t size = static_cast<uint64_t>(fs::file_size(*resolved, ec));
  if (ec) {
    fail("sending", 0, "could not read the file");
    return;
  }
  if (size > kMaxFileBytes) {
    fail("sending", 0, "file is larger than 8 GiB");
    return;
  }
  download item;
  item.id = next_id_++;
  item.name = resolved->filename().string();
  item.path = resolved->string();
  item.size = size;
  item.in.open(item.path, std::ios::binary);
  if (!item.in) {
    fail("sending", 0, "could not open the file");
    return;
  }
  const uint32_t id = item.id;
  downloads_.emplace(id, std::move(item));
  uvcpp_json j;
  j["type"] = "file";
  j["action"] = "sending";
  j["ok"] = true;
  j["id"] = id;
  j["name"] = resolved->filename().string();
  j["size"] = size;
  send_json_(dump_json(j));
}

void file_transfer::pull(uint32_t id) {
  auto it = downloads_.find(id);
  if (it == downloads_.end()) {
    fail("eof", id, "unknown transfer");
    return;
  }
  download& item = it->second;
  if (item.sent >= item.size) {
    send_simple("eof", id);
    downloads_.erase(it);
    return;
  }
  const std::size_t want = static_cast<std::size_t>(
      std::min<uint64_t>(kChunkBytes, item.size - item.sent));
  std::vector<uint8_t> buffer(want);
  item.in.read(reinterpret_cast<char*>(buffer.data()),
               static_cast<std::streamsize>(want));
  const std::streamsize got = item.in.gcount();
  if (got <= 0) {
    fail("eof", id, "could not read the file");
    downloads_.erase(it);
    return;
  }
  if (!send_chunk_(id, item.sent, buffer.data(),
                   static_cast<std::size_t>(got))) {
    // Keep the offset unchanged so the client can retry after transport
    // backpressure clears. The file stream has consumed the bytes, so rewind.
    item.in.clear();
    item.in.seekg(static_cast<std::streamoff>(item.sent), std::ios::beg);
    return;
  }
  item.sent += static_cast<uint64_t>(got);
  if (item.sent >= item.size) {
    send_simple("eof", id);
    downloads_.erase(it);
  }
}

void file_transfer::drop_upload(uint32_t id, bool remove_file) {
  auto it = uploads_.find(id);
  if (it == uploads_.end()) return;
  it->second.out.close();
  if (remove_file) {
    std::error_code ec;
    fs::remove(it->second.temp_path, ec);
  }
  uploads_.erase(it);
}

std::vector<std::string> file_transfer::resolve_files(
    const std::vector<std::string>& paths) const {
  std::vector<std::string> out;
  out.reserve(paths.size());
  for (const std::string& raw : paths) {
    const std::optional<fs::path> resolved = resolve_path(raw);
    if (!resolved) continue;
    std::error_code ec;
    if (!fs::is_regular_file(*resolved, ec)) continue;
    out.push_back(resolved->string());
    if (out.size() >= 256) break;
  }
  return out;
}

void file_transfer::close_all() {
  for (auto& item : uploads_) {
    item.second.out.close();
    std::error_code ec;
    fs::remove(item.second.temp_path, ec);
  }
  uploads_.clear();
  for (auto& item : downloads_) item.second.in.close();
  downloads_.clear();
}

}  // namespace molink
