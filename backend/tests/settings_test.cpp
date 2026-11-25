// Settings round-trip for the host-side port-forward opt-in. The HTTP handler
// in api.cpp parses `port_forward_enabled` into auth_state's setter and reads
// it back through settings(), so exercising the setter plus the persisted
// file covers both directions of /api/settings for this field.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "auth_state.h"
#include "settings.h"

namespace {

namespace fs = std::filesystem;

int checks = 0;
int failures = 0;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

void set_config_dir(const fs::path& dir) {
#if defined(_WIN32)
  _putenv_s("MOLINK_CONFIG_DIR", dir.string().c_str());
#else
  setenv("MOLINK_CONFIG_DIR", dir.string().c_str(), 1);
#endif
}

fs::path make_temp_dir() {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path dir = fs::temp_directory_path() /
                       ("molink-settings-" + std::to_string(stamp));
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  return dir;
}

std::string read_text(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

void test_port_forward_flag_round_trips(const fs::path& dir) {
  const fs::path settings_path = dir / "settings";

  molink::app_settings seed;
  seed.device_name = "settings-test";
  seed.long_term_code = "Test1234";
  molink::save_settings(seed);

  // The core constructs auth_state from load_settings(); the API handler then
  // calls the same setter and reads settings() back for its JSON response.
  molink::auth_state auth(molink::load_settings());
  expect(!auth.settings().port_forward_enabled,
         "port forwarding is off by default");

  auth.set_port_forward_enabled(true);
  expect(auth.settings().port_forward_enabled,
         "the setter flips the value the GET response reads");
  expect(contains(read_text(settings_path), "port_forward_enabled=1\n"),
         "the settings file records port_forward_enabled=1");

  const molink::app_settings reloaded = molink::load_settings();
  expect(reloaded.port_forward_enabled,
         "true survives a save/load round trip");
  expect(reloaded.device_name == "settings-test",
         "unrelated settings survive the rewrite");

  auth.set_port_forward_enabled(false);
  expect(!auth.settings().port_forward_enabled,
         "the setter can turn it off again");
  expect(contains(read_text(settings_path), "port_forward_enabled=0\n"),
         "the settings file records port_forward_enabled=0");
  expect(!molink::load_settings().port_forward_enabled,
         "false survives a save/load round trip");
}

void test_one_setting_does_not_reset_another(const fs::path& dir) {
  const fs::path settings_path = dir / "settings";

  molink::app_settings seed;
  seed.device_name = "settings-test-2";
  seed.long_term_code = "Test1234";
  molink::save_settings(seed);

  molink::auth_state auth(molink::load_settings());
  auth.set_terminal_enabled(true);
  auth.set_port_forward_enabled(true);

  const molink::app_settings reloaded = molink::load_settings();
  expect(reloaded.port_forward_enabled && reloaded.terminal_enabled,
         "port forwarding and the terminal opt-in coexist");
  expect(contains(read_text(settings_path), "port_forward_enabled=1\n"),
         "both flags are written to the same settings file");
  expect(contains(read_text(settings_path), "terminal_enabled=1\n"),
         "the terminal flag is not clobbered by the port-forward save");
}

}  // namespace

int main() {
  const fs::path dir = make_temp_dir();
  set_config_dir(dir);
  test_port_forward_flag_round_trips(dir);
  test_one_setting_does_not_reset_another(dir);
  std::error_code ec;
  fs::remove_all(dir, ec);
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
