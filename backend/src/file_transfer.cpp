#include "file_transfer.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <webapp/uvcpp_web_json.h>

#include "diag_log.h"
#include "download_root.h"
#include "directory_listing.h"
#include "sha256.h"
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

std::string errno_text() {
  const int value = errno;
  return value == 0 ? std::string("unknown error") : std::strerror(value);
}

/// A short random tag for a temporary file name, so two sessions (or two
/// cores) can never mistake each other's `.part` file for their own.
std::string random_hex_tag(std::size_t digits) {
  static const char kHex[] = "0123456789abcdef";
  std::random_device device;
  std::string out;
  out.reserve(digits);
  while (out.size() < digits) {
    const uint32_t value = static_cast<uint32_t>(device());
    for (int shift = 28; shift >= 0 && out.size() < digits; shift -= 4) {
      out.push_back(kHex[(value >> shift) & 0xf]);
    }
  }
  return out;
}

std::string home_directory() {
#if defined(_WIN32)
  // The environment block is Unicode; reading it as narrow text would use the
  // ANSI code page and mangle a Chinese user name on a zh-CN host.
  const wchar_t* profile = _wgetenv(L"USERPROFILE");
  if (profile != nullptr && *profile != L'\0') {
    return path_to_utf8(fs::path(std::wstring(profile)));
  }
  const wchar_t* drive = _wgetenv(L"HOMEDRIVE");
  const wchar_t* path = _wgetenv(L"HOMEPATH");
  if (drive != nullptr && path != nullptr) {
    return path_to_utf8(fs::path(std::wstring(drive) + path));
  }
  return "C:\\";
#else
  const char* home = std::getenv("HOME");
  return home != nullptr && *home != '\0' ? home : "/";
#endif
}

/// Reserves `path` exclusively (`O_EXCL`). The returned handle is only used as
/// a collision test: the actual writes go through `item.out`.
int open_exclusive(const fs::path& path) {
#if defined(_WIN32)
  return _wopen(path.c_str(),
                _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY,
                _S_IREAD | _S_IWRITE);
#else
  return ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
#endif
}

void close_handle(int handle) {
#if defined(_WIN32)
  _close(handle);
#else
  ::close(handle);
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
    if (lowered(path_to_utf8(*r)) != lowered(path_to_utf8(*c))) return false;
  }
  return true;
}

fs::path home_root() {
  std::error_code ec;
  fs::path root = fs::weakly_canonical(path_from_utf8(home_directory()), ec);
  return ec ? path_from_utf8(home_directory()) : root;
}

/// Resolve a peer-supplied path to an absolute path on this machine. `~` and
/// `~/x` are the host user's home; relative paths are relative to home; an
/// empty path is home itself. Absolute paths (POSIX, Windows drive or UNC)
/// are taken as typed: the file channel deliberately has no home sandbox, so
/// the viewer can browse and fetch any file the host user can read. `..` is
/// collapsed lexically and can never climb above a volume root.
std::optional<fs::path> resolve_path(const std::string& raw) {
  if (raw.empty()) return path_from_utf8(home_directory());
  const std::optional<std::string> normalized =
      fslist::normalize_browse_path(raw, home_directory());
  if (!normalized) return std::nullopt;
  return path_from_utf8(*normalized);
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

/// Resolves the viewer's destination folder. Empty means ~/moLink; `~` is the
/// host user's home; relative paths are taken relative to home. Any absolute
/// path the host user can create is allowed when the optional sandbox is off.
/// This only computes the path -- creating it is the caller's job, after the
/// write policy has had a chance to reject it.
std::optional<fs::path> resolve_destination_path(const std::string& requested) {
  fs::path target;
  if (requested.empty()) {
    target = home_root() / "moLink";
  } else if (requested == "~") {
    target = home_root();
  } else if (requested.rfind("~/", 0) == 0 ||
             requested.rfind("~\\", 0) == 0) {
    target = home_root() / path_from_utf8(requested.substr(2));
  } else {
    target = path_from_utf8(requested);
    if (target.is_relative()) target = home_root() / target;
  }
  if (target.empty()) return std::nullopt;
  return target.lexically_normal();
}

/// Expands one configured sandbox root. An empty root is ignored; a relative
/// root is taken from the host home, matching `resolve_destination_path`.
std::optional<fs::path> expand_root(const std::string& raw) {
  std::string text = raw;
  while (!text.empty() &&
         (text.front() == ' ' || text.front() == '\t' ||
          text.front() == '\r' || text.front() == '\n')) {
    text.erase(text.begin());
  }
  while (!text.empty() &&
         (text.back() == ' ' || text.back() == '\t' ||
          text.back() == '\r' || text.back() == '\n')) {
    text.pop_back();
  }
  if (text.empty()) return std::nullopt;
  return resolve_destination_path(text);
}

bool ascii_lower_equal(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    const unsigned char a = static_cast<unsigned char>(left[index]);
    const unsigned char b = static_cast<unsigned char>(right[index]);
    if (std::tolower(a) != std::tolower(b)) return false;
  }
  return true;
}

/// Settings store multiple roots as one line (`;` or newline separated). Each
/// produced string is trimmed by `expand_root`.
std::vector<std::string> split_root_list(const std::string& raw) {
  std::vector<std::string> out;
  std::string current;
  for (char c : raw) {
    if (c == ';' || c == '\n' || c == '\r') {
      out.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  out.push_back(current);
  return out;
}

/// Stable 64-bit FNV-1a, formatted as 16 hex digits. Used only to name a
/// `.part` so two sessions of the same logical upload agree on it.
std::string stable_ident(const std::string& text) {
  uint64_t hash = 1469598103934665603ull;
  for (unsigned char byte : text) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  static const char kHex[] = "0123456789abcdef";
  std::string out(16, '0');
  for (int index = 15; index >= 0; --index) {
    out[static_cast<std::size_t>(index)] = kHex[hash & 0xf];
    hash >>= 4;
  }
  return out;
}

std::string upload_ident(const std::string& resume, const std::string& dir,
                         const std::string& name, uint64_t size) {
  std::string key = resume.empty() ? "path" : "resume";
  key += '\n';
  key += resume;
  key += '\n';
  key += dir;
  key += '\n';
  key += name;
  key += '\n';
  key += std::to_string(size);
  return stable_ident(key);
}

std::string error_text(const std::error_code& ec) {
  return ec ? ec.message() : errno_text();
}

/// SHA-256 of the first `limit` bytes of a file. False when the file is
/// missing or shorter than `limit`: a resume must never accept a short prefix
/// as proof that the retained bytes are the ones the viewer already has.
bool sha256_prefix_file(const std::string& path, uint64_t limit,
                        std::string& hex) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  sha256 hasher;
  char buffer[64 * 1024];
  uint64_t left = limit;
  while (left > 0 && in) {
    const std::size_t want = static_cast<std::size_t>(
        (std::min)(left, static_cast<uint64_t>(sizeof(buffer))));
    in.read(buffer, static_cast<std::streamsize>(want));
    const std::streamsize got = in.gcount();
    if (got <= 0) break;
    hasher.update(buffer, static_cast<std::size_t>(got));
    left -= static_cast<uint64_t>(got);
  }
  if (left != 0) return false;
  hex = hasher.hex_digest();
  return true;
}

/// `.part` files a closed session asked us to keep. The supervisor sweeps
/// them; tests call `file_transfer::sweep_orphans` with an explicit time.
struct orphan_entry {
  std::string path;
  file_transfer::clock::time_point expires;
};

std::mutex& orphan_mutex() {
  static auto* mutex = new std::mutex();
  return *mutex;
}

std::vector<orphan_entry>& orphan_list() {
  static auto* list = new std::vector<orphan_entry>();
  return *list;
}

/// `.part` paths currently owned by a live transfer. A reconnect must never
/// adopt a file that another session is still appending to.
std::mutex& active_mutex() {
  static auto* mutex = new std::mutex();
  return *mutex;
}

std::set<std::string>& active_parts() {
  static auto* parts = new std::set<std::string>();
  return *parts;
}

void mark_part_active(const std::string& path) {
  std::lock_guard<std::mutex> lock(active_mutex());
  active_parts().insert(path);
}

void unmark_part_active(const std::string& path) {
  std::lock_guard<std::mutex> lock(active_mutex());
  active_parts().erase(path);
}

bool part_is_active(const std::string& path) {
  std::lock_guard<std::mutex> lock(active_mutex());
  return active_parts().count(path) != 0;
}

void keep_orphan(const std::string& path, file_transfer::clock::time_point when) {
  std::lock_guard<std::mutex> lock(orphan_mutex());
  for (orphan_entry& entry : orphan_list()) {
    if (entry.path == path) {
      entry.expires = when;
      return;
    }
  }
  orphan_list().push_back(orphan_entry{path, when});
}

void forget_orphan(const std::string& path) {
  std::lock_guard<std::mutex> lock(orphan_mutex());
  auto& list = orphan_list();
  list.erase(std::remove_if(list.begin(), list.end(),
                            [&path](const orphan_entry& entry) {
                              return entry.path == path;
                            }),
             list.end());
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
    out /= path_from_utf8(segment);
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
    : send_json_(std::move(send_json)),
      send_chunk_(std::move(send_chunk)),
      temp_tag_(random_hex_tag(8)) {
  sweep_orphans();
}

file_transfer::~file_transfer() {
  for (auto& entry : uploads_) {
    entry.second.out.close();
    unmark_part_active(entry.second.temp_path);
    std::error_code ec;
    fs::remove(path_from_utf8(entry.second.temp_path), ec);
  }
  uploads_.clear();
  for (auto& entry : downloads_) entry.second.in.close();
  downloads_.clear();
}

void file_transfer::watch_uploads(upload_watcher watcher) {
  upload_watcher_ = std::move(watcher);
}

void file_transfer::set_session_id(std::string id) {
  session_id_ = std::move(id);
}

void file_transfer::set_write_policy(write_policy_provider provider) {
  write_policy_provider_ = std::move(provider);
}

void file_transfer::set_default_destination(
    default_destination_provider provider) {
  default_destination_provider_ = std::move(provider);
}

file_transfer::write_policy file_transfer::current_write_policy() const {
  return write_policy_provider_ ? write_policy_provider_() : write_policy{};
}

bool file_transfer::sandbox_allows(const fs::path& target,
                                   const write_policy& policy,
                                   std::string& root) const {
  if (!policy.enabled) return true;
  std::vector<fs::path> roots;
  for (const std::string& raw : policy.roots) {
    for (const std::string& part : split_root_list(raw)) {
      if (const std::optional<fs::path> expanded = expand_root(part)) {
        roots.push_back(*expanded);
      }
    }
  }
  // An enabled sandbox with no configured root still has a sensible default:
  // the ordinary receive folder.
  if (roots.empty()) roots.push_back(home_root() / "moLink");
  std::error_code ec;
  const fs::path candidate = fs::weakly_canonical(target, ec);
  for (const fs::path& allowed : roots) {
    std::error_code root_ec;
    const fs::path canonical =
        fs::weakly_canonical(allowed, root_ec);
    const fs::path& base = root_ec ? allowed : canonical;
    if (within_root(base, ec ? target : candidate)) {
      root = path_to_utf8(base);
      return true;
    }
  }
  return false;
}

void file_transfer::deny_path(const std::string& action,
                              const fs::path& target,
                              const write_policy& policy) {
  std::vector<std::string> roots;
  for (const std::string& raw : policy.roots) {
    for (const std::string& part : split_root_list(raw)) {
      if (const std::optional<fs::path> expanded = expand_root(part)) {
        roots.push_back(path_to_utf8(*expanded));
      }
    }
  }
  if (roots.empty()) {
    roots.push_back(path_to_utf8(home_root() / "moLink"));
  }
  std::vector<diag_field> fields = {{"action", action},
                                    {"path", path_to_utf8(target)}};
  if (!session_id_.empty()) fields.push_back({"session_id", session_id_});
  diag().log(diag_level::warn, "file.forbidden",
             "write sandbox denied the path", fields);
  uvcpp_json j;
  j["type"] = "file";
  j["action"] = "forbidden_path";
  j["code"] = "forbidden_path";
  j["ok"] = false;
  j["message"] = "write sandbox: path is outside the allowed roots";
  j["path"] = path_to_utf8(target);
  // `root` is the first allowed folder for a one-line prompt; `allowed_roots`
  // carries the whole list. Neither collides with the `roots` volume list a
  // `listing` answer uses.
  j["root"] = roots.front();
  j["allowed_roots"] = uvcpp_json::array();
  for (const std::string& root : roots) j["allowed_roots"].push_back(root);
  send_json_(dump_json(j));
}

void file_transfer::notify_upload(const upload& item, upload_event event,
                                  const std::string& path) {
  if (upload_watcher_) {
    upload_watcher_(item.tag, event, item.id, item.received, item.size, path);
  }
}

void file_transfer::fail(const std::string& action, uint32_t id,
                         const std::string& why) {
  std::vector<diag_field> fields = {{"action", action},
                                    {"id", std::to_string(id)}};
  if (!session_id_.empty()) fields.push_back({"session_id", session_id_});
  diag().log(diag_level::warn, "file.fail", why, fields);
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

bool file_transfer::create_temp_upload_file(upload& item,
                                            const std::string& dir) {
  const fs::path dir_path = path_from_utf8(dir);
  for (int attempt = 0; attempt < 2; ++attempt) {
    const std::string suffix =
        attempt == 0 ? std::string() : "-" + random_hex_tag(4);
    const fs::path candidate =
        dir_path / path_from_utf8(".upload-" + item.ident + "-" + temp_tag_ +
                                  "-" + std::to_string(item.id) + suffix +
                                  ".part");
    const int handle = open_exclusive(candidate);
    if (handle < 0) {
      if (errno != EEXIST) return false;
      continue;
    }
    close_handle(handle);
    std::ofstream out(candidate, std::ios::binary | std::ios::trunc);
    if (!out) {
      std::error_code remove_ec;
      fs::remove(candidate, remove_ec);
      return false;
    }
    item.temp_path = path_to_utf8(candidate);
    item.out = std::move(out);
    mark_part_active(item.temp_path);
    return true;
  }
  return false;
}

bool file_transfer::adopt_temp_upload_file(upload& item,
                                           const std::string& dir,
                                           uint64_t offset,
                                           const std::string& digest) {
  if (offset == 0) return false;
  const fs::path dir_path = path_from_utf8(dir);
  const std::string prefix = ".upload-" + item.ident + "-";
  std::error_code ec;
  fs::path best;
  uint64_t best_size = 0;
  for (const auto& entry : fs::directory_iterator(dir_path, ec)) {
    if (ec) break;
    const std::string name = path_to_utf8(entry.path().filename());
    if (name.rfind(prefix, 0) != 0) continue;
    if (name.size() < 6 ||
        name.compare(name.size() - 5, 5, ".part") != 0) {
      continue;
    }
    std::error_code entry_ec;
    if (!entry.is_regular_file(entry_ec) || entry_ec) continue;
    const std::string path = path_to_utf8(entry.path());
    if (part_is_active(path)) continue;
    const uint64_t disk = entry.file_size(entry_ec);
    if (entry_ec || disk == 0 || disk > item.size) continue;
    if (!digest.empty()) {
      // Only a prefix long enough to cover the digest can prove the file is
      // the one the viewer is talking about. A short `.part` is not a match:
      // the viewer would otherwise resume into bytes we cannot verify.
      if (disk < offset) continue;
      std::string got;
      if (!sha256_prefix_file(path, offset, got)) continue;
      if (!ascii_lower_equal(got, digest)) continue;
    }
    if (disk > best_size) {
      best = entry.path();
      best_size = disk;
    }
  }
  if (best.empty()) return false;
  std::ofstream out(best, std::ios::binary | std::ios::app);
  if (!out) return false;
  item.temp_path = path_to_utf8(best);
  item.out = std::move(out);
  item.received = best_size;
  mark_part_active(item.temp_path);
  forget_orphan(item.temp_path);
  return true;
}

void file_transfer::send_ack(upload& item) {
  uvcpp_json j;
  j["type"] = "file";
  j["action"] = "file-ack";
  j["id"] = item.id;
  j["received"] = item.received;
  send_json_(dump_json(j));
  item.acked = item.received;
  item.chunks_since_ack = 0;
  item.last_ack = clock::now();
}

void file_transfer::send_nack(upload& item) {
  uvcpp_json j;
  j["type"] = "file";
  j["action"] = "file-nack";
  j["id"] = item.id;
  j["received"] = item.received;
  send_json_(dump_json(j));
}

void file_transfer::maybe_ack(upload& item, bool force) {
  if (!force && item.chunks_since_ack < kAckEveryChunks &&
      clock::now() - item.last_ack < kAckInterval) {
    return;
  }
  send_ack(item);
}

void file_transfer::fail_upload(upload& item, const std::string& why) {
  fail("cancel", item.id, why);
  // Additive signal for viewers that can aggregate a failed batch. Older
  // viewers ignore an unknown action and already saw the `cancel` above.
  uvcpp_json j;
  j["type"] = "file";
  j["action"] = "file-failed";
  j["ok"] = false;
  j["id"] = item.id;
  j["name"] = item.name;
  j["message"] = why;
  send_json_(dump_json(j));
}

void file_transfer::offer(const std::string& name, uint64_t size,
                          const std::string& destination, int tag,
                          uint64_t offset, const std::string& digest,
                          const std::string& resume) {
  sweep_orphans();
  if (uploads_.size() >= 8) {
    fail("reject", 0, "too many active uploads");
    return;
  }
  if (size > kMaxFileBytes) {
    fail("reject", 0, "file is larger than 8 GiB");
    return;
  }
  // An empty `dest` is the viewer's "use the host's default". The provider is
  // read here, once: a settings change after the offer must not move an
  // upload that is already running.
  std::string destination_text = destination;
  if (destination_text.empty() && default_destination_provider_) {
    destination_text = default_destination_provider_();
  }
  const std::optional<fs::path> dest_path =
      resolve_destination_path(destination_text);
  if (!dest_path) {
    fail("reject", 0, "could not resolve the receive folder");
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
  if (!prepare_directory(*dest_path, segments, final_dir)) {
    fail("reject", 0, "invalid file path");
    return;
  }
  // The optional sandbox is checked before `create_directories`, so a denied
  // path leaves nothing behind. The second check uses the canonical directory
  // `prepare_directory` produced, which catches a symlink that would have
  // redirected the create.
  const write_policy policy = current_write_policy();
  std::string allowed_root;
  if (!sandbox_allows(*dest_path, policy, allowed_root) ||
      !sandbox_allows(final_dir, policy, allowed_root)) {
    deny_path("offer", final_dir, policy);
    return;
  }
  std::error_code dir_ec;
  fs::create_directories(final_dir, dir_ec);
  if (dir_ec) {
    fail("reject", 0,
         "could not create the destination folder: " + error_text(dir_ec));
    return;
  }
  const uint32_t id = next_id_++;
  upload item;
  item.id = id;
  item.tag = tag;
  item.name = base;
  item.final_dir = path_to_utf8(final_dir);
  item.size = size;
  item.ident = upload_ident(resume, item.final_dir, base, size);
  bool resumed = false;
  if (offset > 0) {
    resumed = adopt_temp_upload_file(item, path_to_utf8(*dest_path), offset,
                                     digest);
  }
  if (!resumed && !create_temp_upload_file(item, path_to_utf8(*dest_path))) {
    fail("reject", 0,
         "could not create " + base + ": " + errno_text());
    return;
  }
  item.last_activity = clock::now();
  item.last_ack = item.last_activity;
  const uint64_t resumed_at = item.received;
  uploads_.emplace(id, std::move(item));
  uvcpp_json j;
  j["type"] = "file";
  j["action"] = "accept";
  j["ok"] = true;
  j["id"] = id;
  j["chunk"] = kChunkBytes;
  // A viewer that understands this paces its chunks and waits for
  // `file-ack`. Older viewers ignore it and keep the legacy fire-and-forget
  // upload; the host accepts both.
  j["ack"] = true;
  j["dir"] = path_to_utf8(*dest_path);
  // Additive resume reply: the host's real contiguous point. A viewer that
  // asked for `offset` but found no matching `.part` gets 0 and restarts.
  j["offset"] = resumed_at;
  j["resumed"] = resumed_at > 0;
  send_json_(dump_json(j));
  std::vector<diag_field> fields = {{"id", std::to_string(id)},
                                    {"name", base},
                                    {"bytes", std::to_string(size)},
                                    {"dir", path_to_utf8(*dest_path)},
                                    {"offset", std::to_string(resumed_at)}};
  if (!session_id_.empty()) fields.push_back({"session_id", session_id_});
  diag().log(diag_level::info, "file.accept", std::string(), fields);
  notify_upload(uploads_.at(id), upload_event::accepted);
}

void file_transfer::mkdir(const std::string& path,
                          const std::string& destination) {
  std::vector<std::string> segments;
  if (!parse_relative_path(path, segments)) {
    fail("mkdir", 0, "invalid folder path");
    return;
  }
  std::string destination_text = destination;
  if (destination_text.empty() && default_destination_provider_) {
    destination_text = default_destination_provider_();
  }
  const std::optional<fs::path> dest_path =
      resolve_destination_path(destination_text);
  if (!dest_path) {
    fail("mkdir", 0, "could not resolve the receive folder");
    return;
  }
  fs::path target;
  if (!prepare_directory(*dest_path, segments, target)) {
    fail("mkdir", 0, "invalid folder path");
    return;
  }
  const write_policy policy = current_write_policy();
  std::string allowed_root;
  if (!sandbox_allows(*dest_path, policy, allowed_root) ||
      !sandbox_allows(target, policy, allowed_root)) {
    deny_path("mkdir", target, policy);
    return;
  }
  std::error_code ec;
  fs::create_directories(target, ec);
  if (ec || !fs::is_directory(target, ec)) {
    fail("mkdir", 0,
         "could not create the folder: " + error_text(ec));
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
  if ((len != 0 && data == nullptr) || offset > item.size ||
      len > item.size - offset) {
    fail_upload(item, "file chunk exceeds the declared size");
    drop_upload(id, true);
    return;
  }
  const auto now = clock::now();
  if (offset > item.received) {
    // Ahead of the contiguous point: a chunk was lost or reordered. Tell the
    // viewer where to resume instead of discarding the whole upload. A
    // viewer that ignores the nack (or never sends the missing range) is
    // failed by the timeout below.
    if (!item.disorder_since) item.disorder_since = now;
    item.last_activity = now;
    if (offset > item.received + kMaxAheadBytes) {
      fail_upload(item, "file chunk is out of order; expected=" +
                            std::to_string(item.received));
      drop_upload(id, true);
      return;
    }
    send_nack(item);
    return;
  }
  if (offset + len <= item.received) {
    // A replay of bytes already on disk (the viewer resent after a lost ack,
    // or two acks crossed). Writing them again would duplicate data, so just
    // answer with the current contiguous point.
    item.disorder_since.reset();
    item.last_activity = now;
    maybe_ack(item, true);
    return;
  }
  // The chunk overlaps the durable prefix: skip the bytes already written and
  // append the rest. This is the resume case after a `file-nack`.
  const std::size_t skip =
      static_cast<std::size_t>(item.received - offset);
  item.out.write(reinterpret_cast<const char*>(data) + skip,
                 static_cast<std::streamsize>(len - skip));
  // Delayed clipboard rendering may read the staging file from another
  // thread; flush each chunk so a reader never waits on this buffer.
  item.out.flush();
  if (!item.out) {
    fail_upload(item, "could not write " + item.name + ": " + errno_text());
    drop_upload(id, true);
    return;
  }
  item.received += len - skip;
  item.disorder_since.reset();
  item.last_activity = now;
  ++item.chunks_since_ack;
  notify_upload(item, upload_event::progress);
  maybe_ack(item, item.received == item.size);
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
    fail_upload(item, "could not write " + item.name + ": " + errno_text());
    drop_upload(id, true);
    return;
  }
  const std::string final_path =
      unique_path(item.final_dir, item.name);
  std::error_code ec;
  fs::rename(path_from_utf8(item.temp_path), path_from_utf8(final_path), ec);
  if (ec) {
    fail_upload(item, "could not finish " + item.name + ": " + ec.message());
    drop_upload(id, true);
    return;
  }
  unmark_part_active(item.temp_path);
  std::vector<diag_field> fields = {{"id", std::to_string(id)},
                                    {"bytes", std::to_string(item.size)},
                                    {"path", final_path}};
  if (!session_id_.empty()) fields.push_back({"session_id", session_id_});
  diag().log(diag_level::info, "file.done", std::string(), fields);
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

namespace {

uvcpp_json root_json(const fslist::root& root) {
  uvcpp_json item;
  item["name"] = root.name;
  item["path"] = root.path;
  item["kind"] = fslist::root_kind_name(root.kind);
  return item;
}

uvcpp_json entry_json(const fslist::entry& entry) {
  uvcpp_json item;
  item["name"] = entry.name;
  item["dir"] = entry.dir;
  item["size"] = entry.dir ? 0 : entry.size;
  item["mtime"] = entry.mtime;
  item["hidden"] = entry.hidden;
  item["system"] = entry.system;
  item["link"] = entry.link;
  if (!entry.error.empty()) item["error"] = entry.error;
  return item;
}

uvcpp_json roots_array(const std::vector<fslist::root>& roots) {
  uvcpp_json array = uvcpp_json::array();
  for (const fslist::root& root : roots) array.push_back(root_json(root));
  return array;
}

}  // namespace

void file_transfer::browse(const std::string& path, bool roots) {
  const std::string home = home_directory();
  const std::vector<fslist::root> volumes = fslist::system_roots(home);
  uvcpp_json volumes_json = roots_array(volumes);
  std::vector<fslist::entry> entries;
  std::string parent;
  std::string display_path;
  bool truncated = false;

  // An empty request (or an explicit `roots`) opens the volume list. The
  // panel returns here with its "up" button once a drive/share root is
  // reached.
  if (!roots && !path.empty()) {
    const std::optional<std::string> normalized =
        fslist::normalize_browse_path(path, home);
    if (!normalized) {
      fail("listing", 0, "invalid path");
      return;
    }
    std::string why;
    const std::optional<fslist::listing> listed =
        fslist::list_directory(*normalized, why);
    if (!listed) {
      fail("listing", 0, why);
      return;
    }
    display_path = listed->path;
    parent = listed->parent;
    entries = listed->entries;
    truncated = listed->truncated;
  } else {
    // The roots are formatted as ordinary directory rows with an absolute
    // `path`, so a click navigates exactly like any other folder.
    for (const fslist::root& root : volumes) {
      fslist::entry item;
      item.name = root.name;
      item.dir = true;
      entries.push_back(std::move(item));
    }
  }

  uvcpp_json list = uvcpp_json::array();
  for (const fslist::entry& entry : entries) {
    uvcpp_json item = entry_json(entry);
    if (display_path.empty()) {
      // Roots view: pair the row with its volume root so a click can go
      // straight there. `entries` is built in the same order as `volumes`.
      const std::size_t index = list.size();
      if (index < volumes.size()) {
        item["path"] = volumes[index].path;
        item["kind"] = fslist::root_kind_name(volumes[index].kind);
        item["root"] = true;
      }
    }
    list.push_back(std::move(item));
  }

  uvcpp_json j;
  j["type"] = "file";
  j["action"] = "listing";
  j["ok"] = true;
  j["path"] = display_path;
  j["parent"] = parent;
  j["root"] = home;
  j["roots"] = std::move(volumes_json);
  j["truncated"] = truncated;
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
    fail("sending", 0, "invalid path");
    return;
  }
  // The optional sandbox covers both directions: a download must also resolve
  // inside the configured roots when it is enabled. Off by default, so the
  // historical "browse and fetch anything readable" path is unchanged.
  const write_policy policy = current_write_policy();
  std::string allowed_root;
  if (!sandbox_allows(*resolved, policy, allowed_root)) {
    deny_path("request", *resolved, policy);
    return;
  }
  std::error_code ec;
  if (!fs::is_regular_file(*resolved, ec)) {
    fail("sending", 0, "not a file");
    return;
  }
  const uint64_t size = static_cast<uint64_t>(fs::file_size(*resolved, ec));
  if (ec) {
    fail("sending", 0, "could not read the file: " + error_text(ec));
    return;
  }
  if (size > kMaxFileBytes) {
    fail("sending", 0, "file is larger than 8 GiB");
    return;
  }
  download item;
  item.id = next_id_++;
  item.name = path_to_utf8(resolved->filename());
  item.path = path_to_utf8(*resolved);
  item.size = size;
  item.last_activity = now;
  item.in.open(*resolved, std::ios::binary);
  if (!item.in) {
    fail("sending", 0, "could not open the file: " + errno_text());
    return;
  }
  const uint32_t id = item.id;
  std::vector<diag_field> send_fields = {{"id", std::to_string(id)},
                                         {"bytes", std::to_string(size)},
                                         {"name", item.name}};
  if (!session_id_.empty()) send_fields.push_back({"session_id", session_id_});
  diag().log(diag_level::info, "file.send", std::string(), send_fields);
  downloads_.emplace(id, std::move(item));
  uvcpp_json j;
  j["type"] = "file";
  j["action"] = "sending";
  j["ok"] = true;
  j["id"] = id;
  j["name"] = item.name;
  j["size"] = size;
  send_json_(dump_json(j));
}

void file_transfer::pull(uint32_t id, clock::time_point now) {
  pull_impl(id, std::nullopt, now);
}

void file_transfer::pull(uint32_t id, uint64_t offset,
                         clock::time_point now) {
  pull_impl(id, offset, now);
}

void file_transfer::pull_impl(uint32_t id, std::optional<uint64_t> offset,
                              clock::time_point now) {
  auto it = downloads_.find(id);
  if (it == downloads_.end()) {
    fail("eof", id, "unknown transfer");
    return;
  }
  download& item = it->second;
  // Every pull is proof that the viewer is still consuming this download.
  item.last_activity = now;
  if (offset && *offset != item.sent) {
    if (*offset > item.size) {
      fail("eof", id, "download offset is past the file end");
      downloads_.erase(it);
      return;
    }
    if (*offset == item.size) {
      // Everything was already saved before the reconnect. There is nothing
      // left to read, and the viewer treats this as a complete transfer.
      send_simple("eof", id);
      downloads_.erase(it);
      return;
    }
    item.sent = *offset;
    item.pending_chunk.clear();
    item.retry_deadline.reset();
    item.in.clear();
    item.in.seekg(static_cast<std::streamoff>(*offset), std::ios::beg);
    if (!item.in) {
      fail("eof", id,
           "could not seek the file: " + errno_text());
      downloads_.erase(it);
      return;
    }
    diag().log(diag_level::info, "file.resume", std::string(),
               {{"id", std::to_string(id)},
                {"offset", std::to_string(*offset)},
                {"session_id", session_id_}});
  }
  if (offset) item.retry_deadline.reset();
  // A pull without an offset keeps the legacy retry semantics: retain the
  // outstanding chunk and let the retry timer resend it. An explicit offset
  // is a fresh request from the viewer, so `send_next` runs immediately.
  if (!item.retry_deadline && send_next(item, now)) {
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
      fail("eof", item.id,
           "could not read the file: " + errno_text());
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
    upload& item = current->second;
    const auto id = current->first;
    if (item.disorder_since && now - *item.disorder_since >= kOrderTimeout) {
      fail_upload(item, "file chunk is out of order; expected=" +
                            std::to_string(item.received));
      drop_upload(id, true);
      continue;
    }
    if (item.complete_deadline && now >= *item.complete_deadline) {
      fail_upload(item, "file ended early; expected=" +
                            std::to_string(item.received));
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
      fail_upload(current->second, "could not schedule file receive timeout");
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
  unmark_part_active(it->second.temp_path);
  forget_orphan(it->second.temp_path);
  if (remove_file) {
    std::error_code ec;
    fs::remove(path_from_utf8(it->second.temp_path), ec);
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
    out.push_back(path_to_utf8(*resolved));
    if (out.size() >= 256) break;
  }
  return out;
}

void file_transfer::close_all() {
  const clock::time_point now = clock::now();
  for (auto& entry : uploads_) {
    upload& item = entry.second;
    item.out.close();
    unmark_part_active(item.temp_path);
    // An unfinished upload is the resume case: keep its bytes for a short
    // grace period. A zero-byte attempt has nothing worth keeping. Note that
    // `received == size` also lands here: all bytes arrived but `complete`
    // never did (the viewer disconnected), and a reconnect can commit the
    // existing `.part` without resending anything.
    if (item.received > 0) {
      keep_orphan(item.temp_path, now + kResumeGrace);
    } else {
      std::error_code ec;
      fs::remove(path_from_utf8(item.temp_path), ec);
    }
  }
  uploads_.clear();
  for (auto& entry : downloads_) entry.second.in.close();
  downloads_.clear();
}

void file_transfer::sweep_orphans(clock::time_point now) {
  std::vector<std::string> expired;
  {
    std::lock_guard<std::mutex> lock(orphan_mutex());
    auto& list = orphan_list();
    for (auto it = list.begin(); it != list.end();) {
      if (it->expires <= now && !part_is_active(it->path)) {
        expired.push_back(it->path);
        it = list.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (const std::string& path : expired) {
    std::error_code ec;
    fs::remove(path_from_utf8(path), ec);
  }
}

}  // namespace molink
