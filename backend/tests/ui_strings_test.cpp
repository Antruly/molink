// Native string table: every key must exist in both languages, the stored
// language tag must round-trip, and the language document must survive a
// write/read cycle on disk.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>

#include "ui_strings.h"

namespace {

namespace fs = std::filesystem;
using molink::ui_strings::entry;
using molink::ui_strings::language;

int checks = 0;
int failures = 0;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

fs::path make_temp_dir() {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path dir = fs::temp_directory_path() /
                       ("molink-ui-strings-" + std::to_string(stamp));
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  return dir;
}

std::string read_text(const fs::path& path) {
  FILE* file = std::fopen(path.string().c_str(), "rb");
  if (file == nullptr) return std::string();
  std::string body;
  char buffer[1024];
  std::size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    body.append(buffer, got);
  }
  std::fclose(file);
  return body;
}

void test_every_key_has_both_languages() {
  std::size_t count = 0;
  const entry* table = molink::ui_strings::entries(&count);
  expect(count > 40, "the table contains the migrated UI strings");
  std::set<std::string> seen;
  for (std::size_t i = 0; i < count; ++i) {
    const entry& item = table[i];
    expect(item.key != nullptr && item.key[0] != '\0',
           "every table row has a key");
    expect(item.en != nullptr && item.en[0] != '\0',
           "every key has an English value");
    expect(item.zh_cn != nullptr && item.zh_cn[0] != '\0',
           "every key has a Chinese value");
    if (item.key != nullptr) {
      expect(seen.insert(item.key).second, "keys are unique");
      expect(molink::ui_strings::has_key(item.key),
             "has_key agrees with the table");
    }
  }
}

void test_known_values_match_the_web_wording() {
  expect(std::wstring(molink::ui_strings::text(language::zh_cn,
                                                "session.disconnect")) ==
             L"断开",
         "disconnect matches the zh-CN locale");
  expect(std::wstring(molink::ui_strings::text(language::en,
                                                "session.disconnect")) ==
             L"Disconnect",
         "disconnect matches the en locale");
  expect(std::wstring(molink::ui_strings::text(language::zh_cn, "tray.exit")) ==
             L"退出",
         "the tray exit label is Chinese by default");
  expect(std::wstring(molink::ui_strings::text(language::en, "tray.exit")) ==
             L"Exit",
         "the tray exit label follows English");
  expect(std::wstring(molink::ui_strings::text(language::en, "missing.key"))
                 .empty(),
         "an unknown key returns an empty string");
}

void test_language_tags_round_trip() {
  language parsed = language::en;
  expect(molink::ui_strings::parse_tag("zh-CN", &parsed) &&
             parsed == language::zh_cn,
         "the Web's zh-CN tag parses");
  expect(molink::ui_strings::parse_tag("en", &parsed) && parsed == language::en,
         "the Web's en tag parses");
  expect(molink::ui_strings::parse_tag("EN-us", &parsed) &&
             parsed == language::en,
         "tags are case-insensitive and may carry a region");
  expect(molink::ui_strings::parse_tag("zh-Hans", &parsed) &&
             parsed == language::zh_cn,
         "any Chinese variant maps to zh-CN");
  expect(!molink::ui_strings::parse_tag("de", &parsed),
         "an unsupported tag is rejected");
  expect(molink::ui_strings::language_or("", language::zh_cn) ==
             language::zh_cn,
         "an empty stored value keeps the fallback");
  expect(std::string(molink::ui_strings::tag(language::zh_cn)) == "zh-CN" &&
             std::string(molink::ui_strings::tag(language::en)) == "en",
         "the stored tags match the Web");
}

void test_language_document_round_trips() {
  const fs::path dir = make_temp_dir();
  const fs::path file = dir / "ui-native.json";
  language loaded = language::zh_cn;

  expect(molink::ui_strings::save_file(file.wstring(), language::en),
         "the language file is written");
  expect(molink::ui_strings::load_file(file.wstring(), &loaded) &&
             loaded == language::en,
         "the saved English preference is read back");
  expect(read_text(file).find("\"language\": \"en\"") != std::string::npos,
         "the file stores the Web's language key");

  expect(molink::ui_strings::save_file(file.wstring(), language::zh_cn),
         "the preference can be overwritten");
  expect(molink::ui_strings::load_file(file.wstring(), &loaded) &&
             loaded == language::zh_cn,
         "the saved Chinese preference is read back");

  std::error_code ec;
  fs::remove_all(dir, ec);
}

void test_missing_and_malformed_files_keep_the_fallback() {
  const fs::path dir = make_temp_dir();
  const fs::path missing = dir / "absent.json";
  language loaded = language::zh_cn;
  expect(!molink::ui_strings::load_file(missing.wstring(), &loaded),
         "a missing language file reports failure");
  expect(loaded == language::zh_cn,
         "a missing file leaves the current language alone");

  const fs::path broken = dir / "broken.json";
  {
    FILE* file = std::fopen(broken.string().c_str(), "wb");
    expect(file != nullptr, "the malformed fixture is created");
    if (file != nullptr) {
      const char body[] = "{\"language\": 42}";
      std::fwrite(body, 1, sizeof(body) - 1, file);
      std::fclose(file);
    }
  }
  loaded = language::en;
  expect(!molink::ui_strings::load_file(broken.wstring(), &loaded),
         "a malformed language value reports failure");
  expect(loaded == language::en,
         "a malformed file leaves the current language alone");

  std::error_code ec;
  fs::remove_all(dir, ec);
}

}  // namespace

int main() {
  test_every_key_has_both_languages();
  test_known_values_match_the_web_wording();
  test_language_tags_round_trip();
  test_language_document_round_trips();
  test_missing_and_malformed_files_keep_the_fallback();
  if (failures != 0) {
    std::fprintf(stderr, "%d of %d checks failed\n", failures, checks);
    return 1;
  }
  std::printf("ui_strings: %d checks passed\n", checks);
  return 0;
}
