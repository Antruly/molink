#include "directory_listing.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <string>
#include <system_error>
#include <utility>

#include "util.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace molink {
namespace fslist {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kMaxPathChars = 4096;
/// A pathological path with thousands of components would make every later
/// Win32 call fail (and is never a real folder). This also bounds how deep a
/// chain of symlinks/junctions a user can navigate.
constexpr std::size_t kMaxComponents = 128;

bool has_illegal_chars(const std::string& text) {
  for (unsigned char c : text) {
    if (c == 0 || c < 0x20 || c == 0x7f) return true;
  }
  return false;
}

std::string lowered(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return value;
}

/// Windows forbids these in a file name (a drive colon is handled before the
/// segment check). Slashes are separators, never part of a segment.
bool has_windows_name_chars(const std::string& segment) {
  for (char c : segment) {
    if (c == '<' || c == '>' || c == ':' || c == '"' || c == '|' ||
        c == '?' || c == '*') {
      return true;
    }
  }
  return false;
}

std::vector<std::string> split_path(const std::string& text, bool windows) {
  const char* separators = windows ? "/\\" : "/";
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find_first_of(separators, start);
    parts.push_back(text.substr(
        start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return parts;
}

std::string join(const std::vector<std::string>& parts, char separator) {
  std::string out;
  for (const std::string& part : parts) {
    if (!out.empty()) out.push_back(separator);
    out += part;
  }
  return out;
}

/// Collapses `.`/`..`/empty segments. `..` at the root is ignored rather than
/// rejected: `cd /..` is `/` in every file manager.
std::vector<std::string> collapse(const std::vector<std::string>& input) {
  std::vector<std::string> out;
  for (const std::string& part : input) {
    if (part.empty() || part == ".") continue;
    if (part == "..") {
      if (!out.empty()) out.pop_back();
      continue;
    }
    out.push_back(part);
  }
  return out;
}

std::string describe_error(const std::error_code& ec) {
  if (ec == std::errc::permission_denied) return "permission denied";
  if (ec == std::errc::no_such_file_or_directory) return "no such folder";
  if (ec == std::errc::not_a_directory) return "not a folder";
  if (ec == std::errc::too_many_symbolic_link_levels) {
    return "too many symbolic links";
  }
  return "could not read the folder";
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

#if defined(_WIN32)
struct win_attributes {
  bool hidden = false;
  bool system = false;
  bool reparse = false;
  bool valid = false;
};

win_attributes read_win_attributes(const fs::path& path) {
  win_attributes out;
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) return out;
  out.valid = true;
  out.hidden = (attributes & FILE_ATTRIBUTE_HIDDEN) != 0;
  out.system = (attributes & FILE_ATTRIBUTE_SYSTEM) != 0;
  out.reparse = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
  return out;
}
#endif

entry read_entry(const fs::directory_entry& item) {
  entry out;
  out.name = path_to_utf8(item.path().filename());
  if (out.name.empty() && item.path().has_root_path()) {
    out.name = native_path(item.path());
  }
  out.hidden = !out.name.empty() && out.name.front() == '.';
  std::error_code link_ec;
  const bool symlink = item.is_symlink(link_ec);
  out.link = symlink && !link_ec;
#if defined(_WIN32)
  const win_attributes attributes = read_win_attributes(item.path());
  if (attributes.valid) {
    out.hidden = out.hidden || attributes.hidden;
    out.system = attributes.system;
    out.link = out.link || attributes.reparse;
  }
#endif
  std::error_code status_ec;
  const fs::file_status status = item.status(status_ec);
  if (status_ec) {
    out.error = "unreadable";
    out.mtime = file_mtime_ms(item.path());
    return out;
  }
  out.dir = fs::is_directory(status);
  if (!out.dir) {
    std::error_code size_ec;
    const auto size = item.file_size(size_ec);
    if (size_ec) {
      out.error = "unreadable";
    } else {
      out.size = static_cast<uint64_t>(size);
    }
  }
  out.mtime = file_mtime_ms(item.path());
  return out;
}

}  // namespace

const char* root_kind_name(root_kind kind) {
  switch (kind) {
    case root_kind::fixed:
      return "fixed";
    case root_kind::removable:
      return "removable";
    case root_kind::network:
      return "network";
    case root_kind::cdrom:
      return "cdrom";
    case root_kind::ram:
      return "ram";
    case root_kind::home:
      return "home";
    case root_kind::unknown:
      break;
  }
  return "unknown";
}

std::vector<root> roots_from_drives(
    uint32_t mask, const std::function<int(int)>& drive_type,
    const std::string& home) {
  std::vector<root> out;
  for (int index = 0; index < 26; ++index) {
    if ((mask & (1u << index)) == 0) continue;
    const int type = drive_type != nullptr ? drive_type(index) : 0;
    root item;
    const char letter = static_cast<char>('A' + index);
    item.name = std::string(1, letter) + ":";
    item.path = item.name + "\\";
    switch (type) {
      case 2:  // DRIVE_REMOVABLE
        item.kind = root_kind::removable;
        break;
      case 3:  // DRIVE_FIXED
        item.kind = root_kind::fixed;
        break;
      case 4:  // DRIVE_REMOTE (mapped network drive)
        item.kind = root_kind::network;
        break;
      case 5:  // DRIVE_CDROM
        item.kind = root_kind::cdrom;
        break;
      case 6:  // DRIVE_RAMDISK
        item.kind = root_kind::ram;
        break;
      default:
        item.kind = root_kind::unknown;
        break;
    }
    out.push_back(std::move(item));
  }
  if (!home.empty()) {
    root shortcut;
    shortcut.name = "Home";
    shortcut.path = home;
    shortcut.kind = root_kind::home;
    out.push_back(std::move(shortcut));
  }
  return out;
}

std::vector<root> system_roots(const std::string& home) {
#if defined(_WIN32)
  const DWORD mask = GetLogicalDrives();
  return roots_from_drives(
      static_cast<uint32_t>(mask),
      [](int index) {
        wchar_t drive[4] = {static_cast<wchar_t>(L'A' + index), L':', L'\\',
                            0};
        return static_cast<int>(GetDriveTypeW(drive));
      },
      home);
#else
  std::vector<root> out;
  out.push_back({"/", "/", root_kind::fixed});
  if (!home.empty() && home != "/") {
    out.push_back({"Home", home, root_kind::home});
  }
  return out;
#endif
}

std::optional<std::string> normalize_posix_path(const std::string& raw_in,
                                                const std::string& home) {
  if (raw_in.empty() || raw_in.size() > kMaxPathChars ||
      has_illegal_chars(raw_in)) {
    return std::nullopt;
  }
  std::string raw = raw_in;
  const std::string base = home.empty() ? "/" : home;
  if (raw == "~") {
    raw = base;
  } else if (raw.rfind("~/", 0) == 0) {
    raw = base + "/" + raw.substr(2);
  } else if (raw.front() != '/') {
    raw = base + "/" + raw;
  }
  std::vector<std::string> parts = collapse(split_path(raw, false));
  if (parts.size() > kMaxComponents) return std::nullopt;
  return "/" + join(parts, '/');
}

std::optional<std::string> normalize_windows_path(const std::string& raw_in,
                                                  const std::string& home) {
  if (raw_in.empty() || raw_in.size() > kMaxPathChars ||
      has_illegal_chars(raw_in)) {
    return std::nullopt;
  }
  std::string raw = raw_in;
  const std::string base = home.empty() ? "C:\\" : home;
  if (raw == "~") {
    raw = base;
  } else if (raw.rfind("~/", 0) == 0 || raw.rfind("~\\", 0) == 0) {
    raw = base + "\\" + raw.substr(2);
  }

  const auto separator = [](char c) { return c == '/' || c == '\\'; };

  // Device paths (`\\?\C:\...`) and named pipes are not browsable folders;
  // they would also bypass the lexical normalisation below.
  if (raw.rfind("\\\\?\\", 0) == 0 || raw.rfind("\\\\.\\", 0) == 0) {
    return std::nullopt;
  }

  std::string prefix;
  std::string rest;
  bool unc = false;

  if (raw.size() >= 2 && separator(raw[0]) && separator(raw[1])) {
    // UNC: \\server\share[\rest]
    const std::vector<std::string> parts = split_path(raw.substr(2), true);
    if (parts.size() < 2 || parts[0].empty() || parts[1].empty() ||
        has_windows_name_chars(parts[0]) ||
        has_windows_name_chars(parts[1])) {
      return std::nullopt;
    }
    unc = true;
    prefix = "\\\\" + parts[0] + "\\" + parts[1];
    if (parts.size() > 2) {
      const std::vector<std::string> tail(parts.begin() + 2, parts.end());
      rest = join(tail, '\\');
    }
  } else if (raw.size() >= 3 &&
             std::isalpha(static_cast<unsigned char>(raw[0])) &&
             raw[1] == ':' && separator(raw[2])) {
    prefix = std::string(1, static_cast<char>(
                                std::toupper(static_cast<unsigned char>(
                                    raw[0])))) +
             ":\\";
    rest = raw.substr(3);
  } else if (raw.size() == 2 &&
             std::isalpha(static_cast<unsigned char>(raw[0])) &&
             raw[1] == ':') {
    // `C:foo` names the current directory of that drive, which this API
    // cannot know; require the separator.
    return std::nullopt;
  } else if (separator(raw.front())) {
    // A single leading separator means the current drive's root. Use the
    // home drive, or C: when there is no home.
    char drive = 'C';
    if (home.size() >= 2 && std::isalpha(static_cast<unsigned char>(home[0])) &&
        home[1] == ':') {
      drive = static_cast<char>(
          std::toupper(static_cast<unsigned char>(home[0])));
    }
    prefix = std::string(1, drive) + ":\\";
    rest = raw.substr(1);
  } else {
    // Relative to the user's home.
    std::string joined = base;
    if (!joined.empty() && !separator(joined.back())) joined.push_back('\\');
    joined += raw;
    return normalize_windows_path(joined, home);
  }

  std::vector<std::string> parts = split_path(rest, true);
  // `..` still means "pop the previous real component"; it cannot climb above
  // the drive or share prefix that was already established.
  std::vector<std::string> clean;
  for (const std::string& part : parts) {
    if (part.empty() || part == ".") continue;
    if (part == "..") {
      if (!clean.empty()) clean.pop_back();
      continue;
    }
    if (has_windows_name_chars(part)) return std::nullopt;
    clean.push_back(part);
  }
  if (clean.size() > kMaxComponents) return std::nullopt;

  if (unc) {
    if (clean.empty()) return prefix;
    return prefix + "\\" + join(clean, '\\');
  }
  return prefix + join(clean, '\\');
}

std::optional<std::string> normalize_browse_path(const std::string& raw,
                                                 const std::string& home) {
#if defined(_WIN32)
  return normalize_windows_path(raw, home);
#else
  return normalize_posix_path(raw, home);
#endif
}

std::string native_path(const fs::path& path) {
  std::string out = path_to_utf8(path);
#if defined(_WIN32)
  std::replace(out.begin(), out.end(), '/', '\\');
#endif
  return out;
}

std::optional<listing> list_directory(const std::string& directory,
                                      std::string& error) {
  error.clear();
  std::error_code ec;
  const fs::path dir = path_from_utf8(directory);
  const bool exists = fs::exists(dir, ec);
  if (ec) {
    error = describe_error(ec);
    return std::nullopt;
  }
  if (!exists) {
    error = "no such folder";
    return std::nullopt;
  }
  ec.clear();
  if (!fs::is_directory(dir, ec)) {
    error = ec ? describe_error(ec) : "not a folder";
    return std::nullopt;
  }

  listing out;
  out.path = native_path(dir);
  const fs::path parent = dir.parent_path();
  out.parent =
      (parent.empty() || parent == dir) ? std::string() : native_path(parent);

  // Opening the folder itself must succeed, otherwise `skip_permission_denied`
  // below would turn "you may not read this folder" into an empty listing.
  {
    std::error_code probe_ec;
    fs::directory_iterator probe(dir, probe_ec);
    if (probe_ec) {
      error = describe_error(probe_ec);
      return std::nullopt;
    }
  }
  fs::directory_iterator it(
      dir, fs::directory_options::skip_permission_denied, ec);
  if (ec) {
    error = describe_error(ec);
    return std::nullopt;
  }
  constexpr std::size_t kMaxEntries = 2000;
  for (; it != fs::directory_iterator(); ++it) {
    if (out.entries.size() >= kMaxEntries) {
      out.truncated = true;
      break;
    }
    out.entries.push_back(read_entry(*it));
  }
  std::sort(out.entries.begin(), out.entries.end(),
            [](const entry& a, const entry& b) {
              if (a.dir != b.dir) return a.dir;
              return lowered(a.name) < lowered(b.name);
            });
  return out;
}

walk_result walk_directory(const std::string& directory,
                           std::size_t max_entries, std::size_t max_depth) {
  walk_result out;
  std::error_code ec;
  const fs::path root = path_from_utf8(directory);
  if (!fs::is_directory(root, ec)) return out;

  struct pending {
    fs::path path;
    std::string rel;
    std::size_t depth;
  };
  std::vector<pending> stack;
  stack.push_back({root, std::string(), 0});
  while (!stack.empty() && out.entries.size() < max_entries) {
    pending current = std::move(stack.back());
    stack.pop_back();
    std::error_code iter_ec;
    fs::directory_iterator it(
        current.path, fs::directory_options::skip_permission_denied, iter_ec);
    if (iter_ec) continue;
    for (; it != fs::directory_iterator(); ++it) {
      if (out.entries.size() >= max_entries) {
        out.truncated = true;
        break;
      }
      entry item = read_entry(*it);
      walk_entry row;
      row.path = current.rel.empty() ? item.name : current.rel + "/" + item.name;
      row.dir = item.dir;
      row.size = item.size;
      row.hidden = item.hidden;
      row.link = item.link;
      out.entries.push_back(std::move(row));
      // Never follow a link/reparse point: a junction loop would otherwise
      // recurse until the depth cap on every level.
      if (item.dir && !item.link && current.depth < max_depth) {
        stack.push_back(
            {it->path(), out.entries.back().path, current.depth + 1});
      }
    }
  }
  return out;
}

std::string unique_destination(const std::string& target) {
  std::error_code ec;
  const fs::path wanted = path_from_utf8(target);
  if (!fs::exists(wanted, ec)) return native_path(wanted);
  const fs::path parent = wanted.parent_path();
  const std::string stem = path_to_utf8(wanted.stem());
  const std::string extension = path_to_utf8(wanted.extension());
  for (int index = 1; index < 10000; ++index) {
    const fs::path candidate =
        parent / (stem + " (" + std::to_string(index) + ")" + extension);
    if (!fs::exists(candidate, ec)) return native_path(candidate);
  }
  return native_path(parent / (stem + "-" + std::to_string(
                                              std::chrono::duration_cast<
                                                  std::chrono::milliseconds>(
                                                  std::chrono::system_clock::now()
                                                      .time_since_epoch())
                                                  .count()) +
                              extension));
}

}  // namespace fslist
}  // namespace molink
