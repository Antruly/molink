#include "display_selector.h"

#include <cctype>
#include <cstdlib>
#include <utility>

namespace molink::display {
namespace {

void skip_ws(std::string_view text, std::size_t* pos) {
  while (*pos < text.size() &&
         (text[*pos] == ' ' || text[*pos] == '\t' || text[*pos] == '\r' ||
          text[*pos] == '\n')) {
    ++*pos;
  }
}

bool parse_string(std::string_view text, std::size_t* pos, std::string* out) {
  if (*pos >= text.size() || text[*pos] != '"') return false;
  ++*pos;
  std::string value;
  while (*pos < text.size()) {
    const char c = text[(*pos)++];
    if (c == '"') {
      *out = std::move(value);
      return true;
    }
    if (c != '\\') {
      value.push_back(c);
      continue;
    }
    if (*pos >= text.size()) return false;
    const char escaped = text[(*pos)++];
    switch (escaped) {
      case '"': value.push_back('"'); break;
      case '\\': value.push_back('\\'); break;
      case '/': value.push_back('/'); break;
      case 'b': value.push_back('\b'); break;
      case 'f': value.push_back('\f'); break;
      case 'n': value.push_back('\n'); break;
      case 'r': value.push_back('\r'); break;
      case 't': value.push_back('\t'); break;
      case 'u': {
        if (*pos + 4 > text.size()) return false;
        unsigned code = 0;
        for (int i = 0; i < 4; ++i) {
          const char hex = text[(*pos)++];
          code <<= 4;
          if (hex >= '0' && hex <= '9') {
            code |= static_cast<unsigned>(hex - '0');
          } else if (hex >= 'a' && hex <= 'f') {
            code |= static_cast<unsigned>(hex - 'a' + 10);
          } else if (hex >= 'A' && hex <= 'F') {
            code |= static_cast<unsigned>(hex - 'A' + 10);
          } else {
            return false;
          }
        }
        // UTF-8. Surrogate pairs are uncommon in monitor names; a lone
        // surrogate is encoded as U+FFFD instead of corrupting the string.
        if (code >= 0xD800 && code <= 0xDFFF) code = 0xFFFD;
        if (code < 0x80) {
          value.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
          value.push_back(static_cast<char>(0xC0 | (code >> 6)));
          value.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
          value.push_back(static_cast<char>(0xE0 | (code >> 12)));
          value.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
          value.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        break;
      }
      default: return false;
    }
  }
  return false;
}

/// Finds a key at or after `start`, then parses its value. The first textual
/// match can be a string value, so the search requires a following ':' just
/// like the session client's helpers.
bool find_string_field(std::string_view text, std::string_view key,
                       std::size_t start, std::string* out) {
  const std::string needle = "\"" + std::string(key) + "\"";
  std::size_t at = start;
  while ((at = text.find(needle, at)) != std::string_view::npos) {
    std::size_t pos = at + needle.size();
    skip_ws(text, &pos);
    if (pos < text.size() && text[pos] == ':') {
      ++pos;
      skip_ws(text, &pos);
      return parse_string(text, &pos, out);
    }
    at += needle.size();
  }
  return false;
}

bool find_int_field(std::string_view text, std::string_view key,
                    std::size_t start, int* out) {
  const std::string needle = "\"" + std::string(key) + "\"";
  std::size_t at = start;
  while ((at = text.find(needle, at)) != std::string_view::npos) {
    std::size_t pos = at + needle.size();
    skip_ws(text, &pos);
    if (pos < text.size() && text[pos] == ':') {
      ++pos;
      skip_ws(text, &pos);
      char* end = nullptr;
      const std::string number(text.data() + pos, text.size() - pos);
      const long value = std::strtol(number.c_str(), &end, 10);
      if (end == number.c_str()) return false;
      *out = static_cast<int>(value);
      return true;
    }
    at += needle.size();
  }
  return false;
}

bool find_bool_field(std::string_view text, std::string_view key,
                     std::size_t start, bool* out) {
  const std::string needle = "\"" + std::string(key) + "\"";
  std::size_t at = start;
  while ((at = text.find(needle, at)) != std::string_view::npos) {
    std::size_t pos = at + needle.size();
    skip_ws(text, &pos);
    if (pos < text.size() && text[pos] == ':') {
      ++pos;
      skip_ws(text, &pos);
      if (text.compare(pos, 4, "true") == 0) {
        *out = true;
        return true;
      }
      if (text.compare(pos, 5, "false") == 0) {
        *out = false;
        return true;
      }
      return false;
    }
    at += needle.size();
  }
  return false;
}

/// `text[begin]` is '{'; returns the index of the matching '}' (or npos).
std::size_t object_end(std::string_view text, std::size_t begin) {
  bool in_string = false;
  bool escaped = false;
  for (std::size_t pos = begin; pos < text.size(); ++pos) {
    const char c = text[pos];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c == '"') {
      in_string = true;
    } else if (c == '}') {
      return pos;
    }
  }
  return std::string_view::npos;
}

info parse_item(std::string_view item, int fallback_index) {
  info out;
  out.index = fallback_index;
  find_int_field(item, "index", 0, &out.index);
  find_string_field(item, "name", 0, &out.name);
  find_int_field(item, "x", 0, &out.x);
  find_int_field(item, "y", 0, &out.y);
  find_int_field(item, "width", 0, &out.width);
  find_int_field(item, "height", 0, &out.height);
  find_bool_field(item, "primary", 0, &out.primary);
  return out;
}

}  // namespace

welcome parse_welcome(std::string_view json) {
  welcome out;
  const std::size_t key = json.find("\"displays\"");
  if (key != std::string_view::npos) {
    std::size_t pos = key + 10;
    skip_ws(json, &pos);
    if (pos < json.size() && json[pos] == ':') {
      ++pos;
      skip_ws(json, &pos);
      if (pos < json.size() && json[pos] == '[') {
        ++pos;
        int fallback = 0;
        while (pos < json.size()) {
          skip_ws(json, &pos);
          if (pos < json.size() && json[pos] == ',') {
            ++pos;
            continue;
          }
          if (pos >= json.size() || json[pos] != '{') break;
          const std::size_t end = object_end(json, pos);
          if (end == std::string_view::npos) break;
          out.list.push_back(
              parse_item(json.substr(pos, end - pos + 1), fallback));
          ++fallback;
          pos = end + 1;
        }
      }
    }
  }
  if (!out.list.empty()) out.current = out.list.front().index;
  int current = 0;
  if (find_int_field(json, "display_index", 0, &current)) {
    out.current = current;
    out.has_current = true;
  }
  return out;
}

const info* find(const welcome& state, int index) {
  for (const info& item : state.list) {
    if (item.index == index) return &item;
  }
  return nullptr;
}

switch_plan plan_switch(const welcome& state, int current, int next) {
  switch_plan out;
  out.index = current;
  if (next == current) return out;
  if (!state.list.empty() && find(state, next) == nullptr) return out;
  out.changed = true;
  out.reset_picture = true;
  out.index = next;
  return out;
}

}  // namespace molink::display
