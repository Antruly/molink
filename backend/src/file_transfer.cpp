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

/// The character filter shared by file names and path segments.
std::string clean_file_name(const std::string& name) {
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
  const fs::path dir = home_root() / "moLink";
  std::error_code ec;
  fs::create_directories(dir, ec);
  return dir.string();
}

/// Resolves the viewer's destination folder. Empty means ~/moLink; `~` is the
/// host user's home; relative paths are taken relative to home. Any absolute
/// path the host user can create is allowed -- the host owner decides what the
/// remote side may write.
std::string resolve_destination(const std::string& requested) {
  if (requested.empty()) return receive_directory();
  std::string text = requested;
  fs::path target;
  if (text == "~") {
    target = home_root();
  } else if (text.rfind("~/", 0) == 0 || text.rfind("~\\", 0) == 0) {
    target = home_root() / text.substr(2);
  } else {
    target = fs::path(text);
    if (target.is_relative()) target = home_root() / target;
  }
  std::error_code ec;
  fs::create_directories(target, ec);
  if (ec) return std::string();
  if (!fs::is_directory(target, ec) || ec) return std::string();
  return target.lexically_normal().string();
}

/// Windows reserves these device names in every directory, with or without an
/// extension (`CON.txt` is still the console). A peer-supplied path that names
/// one is rejected on every platform so a file copied from a Linux host cannot
/// become unusable (or alias a device) when it reaches Windows.
bool is_reserved_name(const std::string& segment) {
  std::string name = segment;
  while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
    name.pop_back();
  }
  if (name.empty()) return false;
  const std::size_t dot = name.find('.');
  if (dot != std::string::npos) name.resize(dot);
  std::string upper;
  upper.reserve(name.size());
  for (unsigned char c : name) {
    upper.push_back(static_cast<char>(std::toupper(c)));
  }
  if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL") {
    return true;
  }
  return upper.size() == 4 &&
         (upper.compare(0, 3, "COM") == 0 || upper.compare(0, 3, "LPT") == 0) &&
         upper[3] >= '1' && upper[3] <= '9';
}

/// Parses a peer-supplied path into cleaned segments. Absolute paths (`/x`,
/// `\\server\share`), drive letters (`C:`), empty or `.` segments and `..`
/// are rejected before any filesystem call, so the result can only ever name
/// a relative chain of normal file names.
bool parse_relative_path(const std::string& raw,
                         std::vector<std::string>& segments) {
  segments.clear();
  if (raw.empty() || raw.size() > 4096) return false;
  std::string text = raw;
  for (char& c : text) {
    if (c == '\\') c = '/';
  }
  if (text.front() == '/') return false;
  if (text.size() >= 2 && std::isalpha(static_cast<unsigned char>(text[0])) &&
      text[1] == ':') {
    return false;
  }
  while (!text.empty() && text.back() == '/') text.pop_back();
  if (text.empty()) return false;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('/', start);
    const std::string segment =
        text.substr(start, end == std::string::npos ? std::string::npos
                                                    : end - start);
    if (segment.empty() || segment == "." || segment == "..") return false;
    if (is_reserved_name(segment)) return false;
    const std::string clean = clean_file_name(segment);
    if (clean.empty() || clean == "." || clean == "..") return false;
    segments.push_back(clean);
    if (segments.size() > 64) return false;
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return !segments.empty();
}

/// Resolves `segments` under `root`, refusing any existing symlink in the
/// chain that points outside it. Called before `create_directories`, so the
/// check sees the symlinks that would otherwise redirect the create.
bool prepare_directory(const fs::path& root,
                       const std::vector<std::string>& segments,
                       fs::path& out) {
  std::error_code ec;
  fs::path base = fs::weakly_canonical(root, ec);
  if (ec) base = root;
  out = base;
  for (const std::string& segment : segments) {
    out /= segment;
    std::error_code check_ec;
    const fs::path canonical = fs::weakly_canonical(out, check_ec);
    if (check_ec || !within_root(base, canonical)) return false;
  }
  return true;
}

std::string join_segments(const std::vector<std::string>& segments) {
  std::string out;
  for (const std::string& segment : segments) {
    if (!out.empty()) out += '/';
    out += segment;
  }
  return out;
}

}  // namespace

file_transfer::file_transfer(json_sender send_json, chunk_sender send_chunk)
    : send_json_(std::move(send_json)), send_chunk_(std::move(send_chunk)) {}

file_transfer::~file_transfer() { close_all(); }

void file_transfer::watch_uploads(upload_watcher watcher) {
  upload_watcher_ = std::move(watcher);
}

void file_transfer::notify_upload(const upload& item, upload_event event,
                                  const std::string& path) {
  if (upload_watcher_) {
    upload_watcher_(item.tag, event, item.id, item.received, item.size, path);
  }
}

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

void file_transfer::offer(const std::string& name, uint64_t size,
                          const std::string& destination, int tag) {
  if (uploads_.size() >= 8) {
    fail("reject", 0, "too many active uploads");
    return;
  }
  if (size > kMaxFileBytes) {
    fail("reject", 0, "file is larger than 8 GiB");
    return;
  }
  const std::string dir = resolve_destination(destination);
  if (dir.empty()) {
    fail("reject", 0, "could not create the receive folder");
    return;
  }
  // A folder drag sends `sub/dir/file`; keep the relative parents and create
  // them under the destination. The segment parser rejects absolute paths,
  // drive letters, `..` and reserved device names before anything is created.
  std::vector<std::string> segments;
  if (!parse_relative_path(name, segments)) {
    fail("reject", 0, "invalid file name");
    return;
  }
  const std::string base = segments.back();
  segments.pop_back();
  fs::path final_dir;
  if (!prepare_directory(dir, segments, final_dir)) {
    fail("reject", 0, "invalid file path");
    return;
  }
  std::error_code dir_ec;
  fs::create_directories(final_dir, dir_ec);
  if (dir_ec) {
    fail("reject", 0, "could not create the destination folder");
    return;
  }
  const uint32_t id = next_id_++;
  upload item;
  item.id = id;
  item.tag = tag;
  item.name = base;
  item.final_dir = final_dir.string();
  item.temp_path = (fs::path(dir) / (".upload-" + std::to_string(id) + ".part"))
                       .string();
  item.size = size;
  item.out.open(item.temp_path, std::ios::binary | std::ios::trunc);
  if (!item.out) {
    fail("reject", 0, "could not create the file");
    return;
  }
  item.last_activity = clock::now();
  uploads_.emplace(id, std::move(item));
  uvcpp_json j;
  j["type"] = "file";
  j["action"] = "accept";
  j["ok"] = true;
  j["id"] = id;
  j["chunk"] = kChunkBytes;
  j["dir"] = dir;
  send_json_(dump_json(j));
  notify_upload(uploads_.at(id), upload_event::accepted);
}

void file_transfer::mkdir(const std::string& path,
                          const std::string& destination) {
  std::vector<std::string> segments;
  if (!parse_relative_path(path, segments)) {
    fail("mkdir", 0, "invalid folder path");
    return;
  }
  const std::string dir = resolve_destination(destination);
  if (dir.empty()) {
    fail("mkdir", 0, "could not create the receive folder");
    return;
  }
  fs::path target;
  if (!prepare_directory(dir, segments, target)) {
    fail("mkdir", 0, "invalid folder path");
    return;
  }
  std::error_code ec;
  fs::create_directories(target, ec);
  if (ec || !fs::is_directory(target, ec)) {
    fail("mkdir", 0, "could not create the folder");
    return;
  }
  uvcpp_json j;
  j["type"] = "file";
  j["action"] = "mkdir";
  j["ok"] = true;
  j["path"] = join_segments(segments);
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
  // Delayed clipboard rendering may read the staging file from another
  // thread; flush each chunk so a reader never waits on this buffer.
  item.out.flush();
  if (!item.out) {
    fail("cancel", id, "could not write the file");
    drop_upload(id, true);
    return;
  }
  item.received += len;
  item.last_activity = clock::now();
  notify_upload(item, upload_event::progress);
  if (item.complete_deadline && item.received == item.size) complete(id);
}

void file_transfer::complete(uint32_t id, clock::time_point now) {
  auto it = uploads_.find(id);
  if (it == uploads_.end()) {
    fail("done", id, "unknown transfer");
    return;
  }
  upload& item = it->second;
  if (item.received != item.size) {
    if (!item.complete_deadline) item.complete_deadline = now + kRetryTimeout;
    return;
  }
  item.out.close();
  if (!item.out) {
    fail("done", id, "could not write the file");
    drop_upload(id, true);
    return;
  }
  const std::string final_path =
      unique_path_in(fs::path(item.final_dir), item.name);
  std::error_code ec;
  fs::rename(item.temp_path, final_path, ec);
  if (ec) {
    fail("done", id, "could not finish the file");
    drop_upload(id, true);
    return;
  }
  notify_upload(item, upload_event::done, final_path);
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

void file_transfer::request(const std::string& path,
                            clock::time_point now) {
  // Bound both open files and the work done by a retry tick.
  if (downloads_.size() >= 8) {
    fail("sending", 0, "too many active downloads");
    return;
  }
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
  item.last_activity = now;
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

void file_transfer::pull(uint32_t id, clock::time_point now) {
  auto it = downloads_.find(id);
  if (it == downloads_.end()) {
    fail("eof", id, "unknown transfer");
    return;
  }
  // Every pull is proof that the viewer is still consuming this download.
  it->second.last_activity = now;
  // The wire request has no offset, so retain only one outstanding pull.
  if (!it->second.retry_deadline && send_next(it->second, now)) {
    downloads_.erase(it);
  }
}

bool file_transfer::send_next(download& item, clock::time_point now) {
  if (item.retry_deadline && now >= *item.retry_deadline) {
    fail("eof", item.id, "file send queue did not become writable");
    return true;
  }
  if (item.sent >= item.size) {
    send_simple("eof", item.id);
    return true;
  }
  if (item.pending_chunk.empty()) {
    const std::size_t want = static_cast<std::size_t>(
        std::min<uint64_t>(kChunkBytes, item.size - item.sent));
    item.pending_chunk.resize(want);
    item.in.read(reinterpret_cast<char*>(item.pending_chunk.data()),
                 static_cast<std::streamsize>(want));
    if (item.in.gcount() != static_cast<std::streamsize>(want)) {
      fail("eof", item.id, "could not read the file");
      return true;
    }
  }
  if (!send_chunk_(item.id, item.sent, item.pending_chunk.data(),
                   item.pending_chunk.size())) {
    // Keep the exact bytes for this pull; do not read again or advance.
    if (!item.retry_deadline) item.retry_deadline = now + kRetryTimeout;
    return false;
  }
  item.retry_deadline.reset();
  item.sent += item.pending_chunk.size();
  item.pending_chunk.clear();
  if (item.sent == item.size) {
    send_simple("eof", item.id);
    return true;
  }
  return false;
}

void file_transfer::retry_pending(clock::time_point now) {
  for (auto it = uploads_.begin(); it != uploads_.end();) {
    auto current = it++;
    if (current->second.complete_deadline &&
        now >= *current->second.complete_deadline) {
      const auto id = current->first;
      fail("done", id, "file ended early");
      drop_upload(id, true);
    }
  }
  for (auto it = downloads_.begin(); it != downloads_.end();) {
    if (it->second.retry_deadline && send_next(it->second, now)) {
      it = downloads_.erase(it);
    } else {
      ++it;
    }
  }
  // Reclaim transfers the viewer abandoned without a `cancel` (the file panel
  // was closed or the tab went away). Without this the eight fixed slots fill
  // up and every later download is refused. An idle upload's partial file is
  // removed with the entry; a download only owns a read handle.
  for (auto it = uploads_.begin(); it != uploads_.end();) {
    if (now - it->second.last_activity >= kIdleTimeout) {
      const uint32_t id = it->first;
      ++it;
      drop_upload(id, true);
    } else {
      ++it;
    }
  }
  for (auto it = downloads_.begin(); it != downloads_.end();) {
    if (now - it->second.last_activity >= kIdleTimeout) {
      it = downloads_.erase(it);
    } else {
      ++it;
    }
  }
}

bool file_transfer::has_pending() const {
  return std::any_of(downloads_.begin(), downloads_.end(), [](const auto& entry) {
    return entry.second.retry_deadline.has_value();
  }) || std::any_of(uploads_.begin(), uploads_.end(), [](const auto& entry) {
    return entry.second.complete_deadline.has_value();
  });
}

bool file_transfer::has_transfers() const {
  return !downloads_.empty() || !uploads_.empty();
}

void file_transfer::fail_pending() {
  for (auto it = uploads_.begin(); it != uploads_.end();) {
    auto current = it++;
    if (current->second.complete_deadline) {
      const auto id = current->first;
      fail("done", id, "could not schedule file receive timeout");
      drop_upload(id, true);
    }
  }
  for (auto it = downloads_.begin(); it != downloads_.end();) {
    if (it->second.retry_deadline) {
      fail("eof", it->first, "could not schedule file send retry");
      it = downloads_.erase(it);
    } else {
      ++it;
    }
  }
}

void file_transfer::drop_upload(uint32_t id, bool remove_file) {
  auto it = uploads_.find(id);
  if (it == uploads_.end()) return;
  notify_upload(it->second, upload_event::failed);
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
