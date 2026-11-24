#include "clipboard_sync.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>

namespace molink::clipboard {
namespace {

void append_utf8(std::string& out, char32_t codepoint) {
  if (codepoint <= 0x7F) {
    out.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}

bool is_high_surrogate(char16_t unit) {
  return unit >= 0xD800 && unit <= 0xDBFF;
}

bool is_low_surrogate(char16_t unit) {
  return unit >= 0xDC00 && unit <= 0xDFFF;
}

}  // namespace

std::string utf8_from_utf16(std::u16string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    char32_t codepoint = text[i];
    if (is_high_surrogate(text[i])) {
      if (i + 1 < text.size() && is_low_surrogate(text[i + 1])) {
        codepoint = 0x10000 + ((codepoint - 0xD800) << 10) +
                    (static_cast<char32_t>(text[i + 1]) - 0xDC00);
        ++i;
      } else {
        codepoint = 0xFFFD;
      }
    } else if (is_low_surrogate(text[i])) {
      codepoint = 0xFFFD;
    }
    append_utf8(out, codepoint);
  }
  return out;
}

std::string fnv1a_hex(std::u16string_view text) {
  std::uint32_t hash = 2166136261u;
  for (char16_t unit : text) {
    hash ^= static_cast<std::uint32_t>(unit);
    hash *= 16777619u;
  }
  // JS `toString(16)` has no leading zeros, and it is lower case.
  char buffer[9] = {};
  std::snprintf(buffer, sizeof(buffer), "%x", static_cast<unsigned int>(hash));
  return buffer;
}

std::string json_escape_utf8(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char raw : text) {
    const unsigned char c = static_cast<unsigned char>(raw);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buffer[8] = {};
          std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                        static_cast<unsigned int>(c));
          out += buffer;
        } else {
          out.push_back(raw);
        }
        break;
    }
  }
  return out;
}

std::string clipboard_sync_json(std::u16string_view text) {
  const std::string utf8 = utf8_from_utf16(text);
  std::string out;
  out.reserve(utf8.size() + 64);
  out += "{\"type\":\"clipboard-sync\",\"text\":\"";
  out += json_escape_utf8(utf8);
  out += "\",\"hash\":\"";
  out += fnv1a_hex(text);
  out += "\",\"chain\":\"\"}";
  return out;
}

std::string text_sync::content_key(std::string_view utf8,
                                   std::u16string_view text) {
  // The hash alone is 32 bits; adding the length makes a collision between
  // two different clipboard strings effectively impossible without carrying
  // a copy of every recent selection.
  return fnv1a_hex(text) + ":" + std::to_string(utf8.size());
}

void text_sync::prune(std::uint64_t now_ms) {
  echoes_.erase(
      std::remove_if(echoes_.begin(), echoes_.end(),
                     [now_ms](const echo_entry& entry) {
                       // Same rule as ClipboardEcho: keep an entry while it
                       // is younger than the TTL. A clock that moved backwards
                       // keeps it rather than expiring everything.
                       return now_ms >= entry.at &&
                              now_ms - entry.at >= echo_ttl_ms;
                     }),
      echoes_.end());
}

bool text_sync::has_echo(const std::string& key) {
  return std::any_of(echoes_.begin(), echoes_.end(),
                     [&key](const echo_entry& entry) {
                       return entry.key == key;
                     });
}

decision text_sync::on_local_text(std::u16string_view text,
                                  std::uint64_t now_ms) {
  decision result;
  if (!enabled_) {
    result.reason = skip_reason::disabled;
    return result;
  }
  if (text.empty()) {
    result.reason = skip_reason::empty;
    return result;
  }
  result.text = utf8_from_utf16(text);
  result.bytes = result.text.size();
  if (result.bytes > max_text_bytes) {
    result.reason = skip_reason::too_large;
    return result;
  }

  prune(now_ms);
  const std::string key = content_key(result.text, text);
  if (has_echo(key)) {
    result.reason = skip_reason::echo;
    return result;
  }
  // A local clipboard update is already proof of a new copy. Identical
  // content must still be sent: copying the same password twice is two
  // copies, and the second one used to be dropped here.
  result.send = true;
  return result;
}

void text_sync::on_remote_text(std::u16string_view text,
                               std::uint64_t now_ms) {
  if (text.empty()) return;
  const std::string utf8 = utf8_from_utf16(text);
  // An oversized value can never go back out (on_local_text skips it), so it
  // would only take a slot in the table.
  if (utf8.size() > max_text_bytes) return;

  prune(now_ms);
  const std::string key = content_key(utf8, text);
  const auto existing =
      std::find_if(echoes_.begin(), echoes_.end(),
                   [&key](const echo_entry& entry) { return entry.key == key; });
  if (existing != echoes_.end()) echoes_.erase(existing);
  echoes_.push_back({key, now_ms});
  while (echoes_.size() > echo_max_entries) echoes_.pop_front();
}

void text_sync::clear_echo() { echoes_.clear(); }

}  // namespace molink::clipboard
