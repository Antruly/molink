// Tray / autostart rules: the Windows Run value, the Linux .desktop entry,
// the real-state checks and the close_to_tray default. Pure logic only: this
// test runs on Linux next to the rest of the shell tests.
#include <cstdio>
#include <string>

#include "tray_settings.h"

namespace {

using namespace molink::tray;

int checks = 0;
int failures = 0;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

void expect_eq(const std::string& actual, const std::string& wanted,
               const char* description) {
  ++checks;
  if (actual == wanted) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n  wanted: %s\n  actual: %s\n", description,
               wanted.c_str(), actual.c_str());
}

void test_close_default() {
  expect(default_close_to_tray(true), "autostart on -> close to tray");
  expect(!default_close_to_tray(false), "autostart off -> quit on close");
  expect(effective_close_to_tray(true, false, false),
         "no override, autostart on -> tray");
  expect(!effective_close_to_tray(false, false, true),
         "no override, autostart off -> quit");
  expect(!effective_close_to_tray(true, true, false),
         "explicit false wins over autostart on");
  expect(effective_close_to_tray(false, true, true),
         "explicit true wins over autostart off");
}

void test_windows_run_value() {
  const std::string exe = "C:\\Program Files\\moLink\\moLink.exe";
  expect_eq(windows_run_command(exe),
            "\"C:\\Program Files\\moLink\\moLink.exe\" --tray",
            "the Run value quotes the exe and appends --tray");
  expect_eq(std::string(windows_run_key()),
            "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            "the Run key path matches the requirement");
  expect_eq(std::string(windows_run_value_name()), "moLink",
            "the Run value name is moLink");

  expect(windows_run_command_matches(windows_run_command(exe), exe),
         "the written command matches its own exe");
  expect(windows_run_command_matches(
             "\"C:\\Program Files\\moLink\\moLink.exe\"", exe),
         "the same exe without --tray matches");
  expect(windows_run_command_matches(
             "  \"C:\\Program Files\\moLink\\moLink.exe\" --tray  ", exe),
         "surrounding whitespace is ignored");
  expect(windows_run_command_matches(
             "\"c:/program files/molink/MOLINK.EXE\" --tray", exe),
         "case and separators do not matter on Windows");
  expect(windows_run_command_matches(
             "\"C:\\Program Files\\moLink\\moLink.exe\\\\\" --tray", exe),
         "a trailing separator does not matter");
  expect(windows_run_command_matches(
             "C:\\Program Files\\moLink\\moLink.exe --tray", exe),
         "an unquoted command word still matches when it has no spaces");
  expect(windows_run_command_matches(
             "\"C:\\Program Files\\moLink\\moLink.exe\" --tray --debug", exe),
         "extra arguments after --tray do not matter");
  expect(!windows_run_command_matches(
             "\"C:\\Program Files\\moLink\\moLink-other.exe\" --tray", exe),
         "a different executable does not match");
  expect(!windows_run_command_matches(
             "\"C:\\Program Files\\moLink\" --tray", exe),
         "a directory prefix does not match");
  expect(!windows_run_command_matches("", exe), "an empty value does not match");
  expect(!windows_run_command_matches("\"C:\\x\\moLink.exe", exe),
         "an unterminated quote does not match");
  expect(!windows_run_command_matches("\"C:\\x\\moLink.exe\"",
                                      "C:\\y\\moLink.exe"),
         "a different install path does not match");
  expect(!windows_run_command_matches(windows_run_command(exe), ""),
         "an empty exe path never matches");
}

void test_linux_desktop_entry() {
  const std::string exe = "/usr/bin/moLink";
  const std::string entry = linux_desktop_entry(exe);
  expect(entry.find("[Desktop Entry]\n") == 0,
         "the entry starts with the Desktop Entry group");
  expect(entry.find("Type=Application\n") != std::string::npos,
         "the entry has Type=Application");
  expect(entry.find("Exec=\"/usr/bin/moLink\" --tray\n") != std::string::npos,
         "Exec quotes the exe and supplies --tray");
  expect(entry.find("X-GNOME-Autostart-enabled=true\n") != std::string::npos,
         "the entry explicitly enables GNOME autostart");
  expect(entry.find("Terminal=false\n") != std::string::npos,
         "the entry does not open a terminal");
  expect_eq(std::string(linux_autostart_relative_path()),
            "autostart/molink.desktop",
            "the autostart file lives under ~/.config/autostart");

  const std::string spaced = linux_desktop_entry("/opt/My Apps/moLink");
  expect(spaced.find("Exec=\"/opt/My Apps/moLink\" --tray\n") !=
             std::string::npos,
         "a path with spaces stays one quoted argument");
  const std::string quoted = linux_desktop_entry("/opt/a\"b\\c/moLink");
  expect(quoted.find("Exec=\"/opt/a\\\"b\\\\c/moLink\" --tray\n") !=
             std::string::npos,
         "quotes and backslashes are escaped");
  const std::string percent = linux_desktop_entry("/opt/100%/moLink");
  expect(percent.find("Exec=\"/opt/100%%/moLink\" --tray\n") !=
             std::string::npos,
         "percent signs are escaped for Exec field codes");
}

void test_linux_enabled() {
  const std::string enabled = linux_desktop_entry("/usr/bin/moLink");
  expect(linux_desktop_enabled(true, enabled),
         "the generated file reads back as enabled");
  expect(!linux_desktop_enabled(false, enabled),
         "a missing file means disabled");
  expect(!linux_desktop_enabled(true, ""),
         "an empty file is not enabled");
  expect(!linux_desktop_enabled(
             true,
             "[Desktop Entry]\nX-GNOME-Autostart-enabled=false\n"),
         "an explicit false disables autostart");
  expect(linux_desktop_enabled(
             true,
             "[Desktop Entry]\nX-GNOME-Autostart-enabled = TRUE\n"),
         "whitespace and value case are tolerated");
  expect(linux_desktop_enabled(
             true, "X-GNOME-Autostart-enabled=true\n"),
         "a file without a group header still counts");
  expect(!linux_desktop_enabled(
             true,
             "[Desktop Entry]\nHidden=true\nX-GNOME-Autostart-enabled=true\n"),
         "Hidden=true disables the entry");
  expect(!linux_desktop_enabled(
             true,
             "[Desktop Entry]\n# X-GNOME-Autostart-enabled=true\n"),
         "a commented key does not enable autostart");
  expect(linux_desktop_enabled(
             true,
             "[Desktop Entry]\nX-GNOME-Autostart-enabled=true\n"
             "[Other]\nX-GNOME-Autostart-enabled=false\n"),
         "the first matching key wins and stays enabled");
  expect(!linux_desktop_enabled(
             true,
             "[Other]\nX-GNOME-Autostart-enabled=true\n"
             "[Desktop Entry]\nX-GNOME-Autostart-enabled=false\n"),
         "a key in another group is ignored");
  expect(!linux_desktop_enabled(
             true, "[Desktop Entry]\nX-GNOME-Autostart-enabled=maybe\n"),
         "an unparsable value is not enabled");
  expect(!linux_desktop_enabled(
             true, "[Desktop Entry]\nName=moLink\n"),
         "a file without the key is not enabled by the strict rule");
}

void test_prefs_round_trip() {
  prefs value;
  expect(!value.close_to_tray_set && !value.balloon_seen,
         "a fresh preference store has no overrides");
  set_close_to_tray(value, true);
  mark_balloon_seen(value);
  expect(value.close_to_tray_set && value.close_to_tray && value.balloon_seen,
         "the setters record both prefs");
  const std::string text = serialize_prefs(value);
  expect_eq(text, "close_to_tray=1\ntray_balloon_seen=1\n",
            "the serialized document uses stable key names");
  const prefs loaded = parse_prefs(text);
  expect(loaded.close_to_tray_set && loaded.close_to_tray && loaded.balloon_seen,
         "the document round-trips");

  prefs off;
  set_close_to_tray(off, false);
  const prefs loaded_off = parse_prefs(serialize_prefs(off));
  expect(loaded_off.close_to_tray_set && !loaded_off.close_to_tray,
         "an explicit false is kept, not confused with unset");
  expect(!loaded_off.balloon_seen, "an unset balloon stays unset");

  const prefs commented = parse_prefs(
      "# close_to_tray=1\n; tray_balloon_seen=1\nclose_to_tray=0\n");
  expect(commented.close_to_tray_set && !commented.close_to_tray,
         "comments are ignored and the real key is read");
  expect(!commented.balloon_seen, "a commented balloon key is ignored");

  const prefs spaced =
      parse_prefs(" close_to_tray = yes \ntray_balloon_seen=on\n");
  expect(spaced.close_to_tray_set && spaced.close_to_tray,
         "keys and values are trimmed and yes means true");
  expect(spaced.balloon_seen, "on means true");

  const prefs unknown =
      parse_prefs("something_else=1\nclose_to_tray=maybe\n");
  expect(!unknown.close_to_tray_set,
         "unknown keys and invalid values are ignored");

  expect_eq(serialize_prefs(prefs{}), "",
            "an empty preference store writes nothing");
}

void test_log_line() {
  expect_eq(log_line("hide", true, true),
            "shell.tray action=hide autostart=1 close_to_tray=1",
            "hide log line");
  expect_eq(log_line("show", false, false),
            "shell.tray action=show autostart=0 close_to_tray=0",
            "show log line");
  expect_eq(log_line("quit", true, false),
            "shell.tray action=quit autostart=1 close_to_tray=0",
            "quit log line");
}

}  // namespace

int main() {
  test_close_default();
  test_windows_run_value();
  test_linux_desktop_entry();
  test_linux_enabled();
  test_prefs_round_trip();
  test_log_line();
  std::printf("tray_settings: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
