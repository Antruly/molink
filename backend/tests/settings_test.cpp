// Settings round-trip for the host-side port-forward opt-in. The HTTP handler
// in api.cpp parses `port_forward_enabled` into auth_state's setter and reads
// it back through settings(), so exercising the setter plus the persisted
// file covers both directions of /api/settings for this field.
//
// It also pins the compiled host_only default: false in the full product, true
// when configured with -DMOLINK_HOST_ONLY_BUILD=ON, and always overridable by
// the persisted file after the host-only build has written it.
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

// The host_only default is compiled in: off for the full product, on for the
// host-only product. default_host_only() is the single source of truth, so the
// test asserts both the function and the struct's member initializer.
void test_host_only_default_source() {
  const fs::path dir = make_temp_dir();
  set_config_dir(dir);

  const molink::app_settings fresh;
  expect(fresh.host_only == molink::default_host_only(),
         "app_settings takes host_only from the compiled default");
#if defined(MOLINK_HOST_ONLY_BUILD) && MOLINK_HOST_ONLY_BUILD
  expect(molink::default_host_only(),
         "MOLINK_HOST_ONLY_BUILD=ON makes host_only default true");
#else
  expect(!molink::default_host_only(),
         "a normal build keeps host_only default false");
#endif
  expect(molink::load_settings().host_only == molink::default_host_only(),
         "a missing settings file keeps the compiled default");

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// A settings file can already contain host_only=0: written by the full product
// (the two products share %APPDATA%\moLink) or by a release that predates the
// host-only default. The host-only build must start in host-only mode anyway,
// then honour the user turning it off.
void test_host_only_upgrade_and_opt_out() {
  const fs::path dir = make_temp_dir();
  set_config_dir(dir);
  {
    std::ofstream out(dir / "settings", std::ios::binary | std::ios::trunc);
    out << "device_name=settings-test-upgrade\n";
    out << "host_only=0\n";
  }

  molink::app_settings loaded = molink::load_settings();
  if (molink::default_host_only()) {
    expect(loaded.host_only,
           "the host-only build overrides a pre-host-only host_only=0");
    // The first save records that this build owns the setting. After that, an
    // explicit off is the user's choice and survives a reload.
    loaded.host_only = false;
    molink::save_settings(loaded);
    expect(fs::exists(dir / "host_only_build"),
           "a host-only save records the ownership marker");
    expect(!molink::load_settings().host_only,
           "after the marker, the user's host_only=0 is honoured");
  } else {
    expect(!loaded.host_only, "the full build honours host_only=0");
    loaded.host_only = true;
    molink::save_settings(loaded);
    expect(molink::load_settings().host_only,
           "the full build honours host_only=1");
    expect(!fs::exists(dir / "host_only_build"),
           "the full build never writes the host-only marker");
  }

  std::error_code ec;
  fs::remove_all(dir, ec);
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
  test_host_only_default_source();
  test_host_only_upgrade_and_opt_out();
  std::error_code ec;
  fs::remove_all(dir, ec);
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
