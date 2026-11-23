// Native string table.
//
// The native client cannot load the Web's i18next resources, so the keys it
// shares with the page live here. Key names match
// `frontend/src/locales/{zh-CN,en}.json` where the string exists there; the
// `tray.*` and `toast.*` entries cover shell-only UI. The table is
// Windows-independent so the Linux tests can check completeness.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace molink::ui_strings {

enum class language { en = 0, zh_cn = 1 };

struct entry {
  const char* key;
  const wchar_t* en;
  const wchar_t* zh_cn;
};

/// The full table. `count` receives the number of entries.
const entry* entries(std::size_t* count);

/// The value for `key`, or an empty string when the key is unknown.
const wchar_t* text(language lang, std::string_view key);

/// True when the table owns `key`.
bool has_key(std::string_view key);

/// `"en"` / `"zh-CN"`, the tags the Web stores.
const char* tag(language lang);

/// Parses a stored tag. Accepts the Web's exact values plus any `zh*`/`en*`
/// variant (`zh-Hans`, `en-US`), case-insensitively.
bool parse_tag(std::string_view value, language* out);

/// Parses `value`, or returns `fallback` when it is empty/unknown.
language language_or(std::string_view value, language fallback);

/// Reads `{"language":"..."}` from `path`. Returns false (and leaves `*out`
/// alone) when the file is missing, unreadable, or has no valid language.
bool load_file(const std::wstring& path, language* out);

/// Writes the same one-key document, creating/truncating `path`.
bool save_file(const std::wstring& path, language lang);

}  // namespace molink::ui_strings
