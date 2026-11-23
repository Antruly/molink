#include "clipboard_files.h"

#include <algorithm>
#include <cstdio>

namespace molink::clipboard_files {
namespace {

char ascii_lower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string lower_ascii(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) out.push_back(ascii_lower(c));
  return out;
}

bool same_name(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
  }
  return true;
}

std::string trim_edges(std::string text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '.')) {
    text.erase(text.begin());
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '.')) {
    text.pop_back();
  }
  return text;
}

/// Cut at a UTF-8 code point boundary at or below `limit` bytes.
std::string clamp_utf8(std::string text, std::size_t limit) {
  if (text.size() <= limit) return text;
  std::size_t cut = limit;
  while (cut > 0 &&
         (static_cast<unsigned char>(text[cut]) & 0xc0) == 0x80) {
    --cut;
  }
  text.resize(cut);
  return trim_edges(std::move(text));
}

std::string descriptor_key(std::string_view name, std::uint64_t size) {
  return "desc:" + lower_ascii(name) + '\x1f' + std::to_string(size);
}

}  // namespace

std::string sanitize_name(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  for (const unsigned char c : raw) {
    if (c == 0) return std::string();
    // Path separators and the drive/stream separator would let a crafted
    // remote name choose the destination. Reject instead of silently
    // stripping, so the caller counts the entry as omitted.
    if (c == '/' || c == '\\' || c == ':') return std::string();
    if (c < 0x20 || c == 0x7f) continue;
    if (c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
      continue;
    }
    out.push_back(static_cast<char>(c));
  }
  out = trim_edges(std::move(out));
  if (out.empty() || out == "." || out == "..") return std::string();
  return clamp_utf8(std::move(out), 180);
}

std::string unique_name(std::string_view name, std::vector<std::string>& taken) {
  std::string clean(name);
  auto collides = [&taken](std::string_view candidate) {
    return std::any_of(taken.begin(), taken.end(),
                       [candidate](const std::string& existing) {
                         return same_name(existing, candidate);
                       });
  };
  if (!collides(clean)) {
    taken.push_back(clean);
    return clean;
  }
  // Split at the last dot, like the host's `unique_final_path_locked`.
  const std::size_t dot = clean.find_last_of('.');
  const std::string stem = dot == std::string::npos ? clean : clean.substr(0, dot);
  const std::string extension =
      dot == std::string::npos ? std::string() : clean.substr(dot);
  for (int i = 1; i < 10000; ++i) {
    std::string candidate =
        clamp_utf8(stem + " (" + std::to_string(i) + ")", 180 - extension.size());
    candidate += extension;
    if (!collides(candidate)) {
      taken.push_back(candidate);
      return candidate;
    }
  }
  std::string fallback = clamp_utf8(stem + "-" + std::to_string(taken.size()),
                                    180 - extension.size());
  fallback += extension;
  taken.push_back(fallback);
  return fallback;
}

plan plan_entries(const std::vector<entry>& raw, std::size_t max_count,
                  std::uint64_t max_file, std::uint64_t max_total) {
  plan result;
  std::vector<entry> parsed;
  parsed.reserve(raw.size());
  std::vector<std::string> taken;
  std::size_t invalid = 0;
  for (const entry& item : raw) {
    if (item.id == 0 || item.name.empty()) {
      ++invalid;
      continue;
    }
    const std::string clean = sanitize_name(item.name);
    if (clean.empty()) {
      ++invalid;
      continue;
    }
    entry file = item;
    file.name = unique_name(clean, taken);
    parsed.push_back(std::move(file));
  }

  for (entry& file : parsed) {
    if (result.files.size() >= max_count) break;
    if (file.size > max_file) break;
    if (result.bytes + file.size > max_total) break;
    result.bytes += file.size;
    result.files.push_back(std::move(file));
  }
  result.omitted = invalid + (parsed.size() - result.files.size());
  return result;
}

plan plan_pull_entries(const std::vector<entry>& raw) {
  return plan_entries(raw, max_files, max_file_bytes, max_total_bytes);
}

plan plan_offer_entries(const std::vector<entry>& raw) {
  return plan_entries(raw, max_files, offer_max_total_bytes,
                      offer_max_total_bytes);
}

std::uint64_t chunk_length(std::uint64_t size, std::uint64_t offset) {
  if (offset >= size) return 0;
  return std::min(chunk_bytes, size - offset);
}

chunk_state check_chunk(std::uint32_t status, std::uint64_t chunk_offset,
                        std::uint64_t expected_offset,
                        std::uint64_t expected_length,
                        std::uint64_t byte_length) {
  if (status == 2) return chunk_state::gone;
  if (status != 0) return chunk_state::failed;
  if (chunk_offset != expected_offset || byte_length != expected_length) {
    return chunk_state::incomplete;
  }
  return chunk_state::ok;
}

std::string staging_relative_directory(std::string_view session,
                                       std::uint32_t sequence) {
  std::string clean;
  clean.reserve(session.size());
  for (const unsigned char c : session) {
    const bool safe = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                      (c >= 'A' && c <= 'Z') || c == '.' || c == '_' ||
                      c == '-';
    clean.push_back(safe ? static_cast<char>(c) : '_');
  }
  if (clean.empty()) clean = "session";
  // A directory name that is only dots is `..` on Windows, which would walk
  // out of the clipboard folder. Trim them; the generated session label is
  // ASCII anyway, so a valid label is unaffected.
  while (!clean.empty() && clean.front() == '.') clean.erase(clean.begin());
  while (!clean.empty() && clean.back() == '.') clean.pop_back();
  if (clean.empty()) clean = "session";
  if (clean.size() > 64) clean.resize(64);
  return "clipboard/" + clean + "/" + std::to_string(sequence);
}

std::string normalize_path_key(std::string_view path) {
  std::string out;
  out.reserve(path.size());
  bool previous_slash = false;
  for (char raw : path) {
    char c = ascii_lower(raw);
    if (c == '\\') c = '/';
    if (c == '/') {
      if (previous_slash) continue;
      previous_slash = true;
    } else {
      previous_slash = false;
    }
    out.push_back(c);
  }
  while (!out.empty() && out.back() == '/') out.pop_back();
  return out;
}

void file_echo::prune(std::uint64_t now_ms) {
  entries_.erase(
      std::remove_if(entries_.begin(), entries_.end(),
                     [now_ms](const item& entry) {
                       return now_ms >= entry.at &&
                              now_ms - entry.at >= echo_ttl_ms;
                     }),
      entries_.end());
}

bool file_echo::has(std::string_view key) {
  return std::any_of(entries_.begin(), entries_.end(),
                     [key](const item& entry) { return entry.key == key; });
}

void file_echo::remember(std::string key, std::uint64_t now_ms) {
  prune(now_ms);
  const auto existing =
      std::find_if(entries_.begin(), entries_.end(),
                   [&key](const item& entry) { return entry.key == key; });
  if (existing != entries_.end()) entries_.erase(existing);
  entries_.push_back({std::move(key), now_ms});
  while (entries_.size() > echo_max_entries) entries_.pop_front();
}

void file_echo::remember_path(std::string_view path, std::uint64_t now_ms) {
  const std::string key = normalize_path_key(path);
  if (key.empty()) return;
  remember("path:" + key, now_ms);
}

void file_echo::remember_descriptor(std::string_view name, std::uint64_t size,
                                    std::uint64_t now_ms) {
  if (name.empty()) return;
  remember(descriptor_key(name, size), now_ms);
}

bool file_echo::suppress_path(std::string_view path, std::uint64_t now_ms) {
  prune(now_ms);
  const std::string key = normalize_path_key(path);
  if (key.empty()) return false;
  return has("path:" + key);
}

bool file_echo::suppress_descriptor(std::string_view name, std::uint64_t size,
                                    std::uint64_t now_ms) {
  prune(now_ms);
  if (name.empty()) return false;
  return has(descriptor_key(name, size));
}

}  // namespace molink::clipboard_files
