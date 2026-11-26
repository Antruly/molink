// Tray / autostart settings shared by the three desktop shells.
//
// This header is deliberately free of Win32, GTK and libc-specific calls so
// the backend test build can exercise the rules on Linux. The shells keep the
// OS calls (registry, ~/.config/autostart) and feed their raw values in here.
#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace molink::tray {

// ---------------------------------------------------------------------------
// Close behaviour
// ---------------------------------------------------------------------------

/// The close button enters the tray only when the user asked for it. Before
/// the user ever touches the setting it follows the autostart state: a build
/// that starts at login also stays reachable after its window is closed.
inline bool default_close_to_tray(bool autostart) { return autostart; }

/// The override (if the user ever changed close_to_tray by hand) wins over
/// the autostart-derived default.
inline bool effective_close_to_tray(bool autostart, bool override_present,
                                    bool override_value) {
  return override_present ? override_value : default_close_to_tray(autostart);
}

// ---------------------------------------------------------------------------
// Windows: HKCU\Software\Microsoft\Windows\CurrentVersion\Run
// ---------------------------------------------------------------------------

inline const char* windows_run_key() {
  return "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
}

inline const char* windows_run_value_name() { return "moLink"; }

/// The exact value the shell writes: a quoted executable followed by --tray.
inline std::string windows_run_command(const std::string& exe_path) {
  return "\"" + exe_path + "\" --tray";
}

namespace detail {

inline std::string trim_ascii(std::string text) {
  const auto space = [](unsigned char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' ||
           ch == '\v';
  };
  text.erase(text.begin(),
             std::find_if_not(text.begin(), text.end(), space));
  while (!text.empty() && space(static_cast<unsigned char>(text.back()))) {
    text.pop_back();
  }
  return text;
}

inline char lower_ascii(char ch) {
  return static_cast<char>(
      std::tolower(static_cast<unsigned char>(ch)));
}

/// Compare Windows paths the way the registry should be read: separators and
/// ASCII case do not matter, and a trailing separator is not significant.
inline std::string normalize_windows_path(std::string path) {
  for (char& ch : path) {
    if (ch == '/') ch = '\\';
    ch = lower_ascii(ch);
  }
  const bool drive_root =
      path.size() == 3 && path[1] == ':' && path[2] == '\\';
  while (path.size() > 1 && path.back() == '\\' && !drive_root) {
    path.pop_back();
  }
  return path;
}

}  // namespace detail

/// True when the Run value launches the current executable. Only the command
/// word is compared (`"<exe>" --tray`, `"<exe>"` and an unquoted `<exe>` all
/// match); a different executable, even one with the same prefix, does not.
inline bool windows_run_command_matches(const std::string& value,
                                        const std::string& exe_path) {
  const std::string text = detail::trim_ascii(value);
  if (text.empty() || exe_path.empty()) return false;
  if (text.front() != '"') {
    // Unquoted Run values are common. The executable path may itself contain
    // spaces, so compare the normalised text from the start and require the
    // next byte to end the command word (or the string).
    const std::string command = detail::normalize_windows_path(text);
    const std::string exe = detail::normalize_windows_path(exe_path);
    if (command.size() < exe.size() ||
        command.compare(0, exe.size(), exe) != 0) {
      return false;
    }
    if (command.size() == exe.size()) return true;
    const unsigned char next =
        static_cast<unsigned char>(command[exe.size()]);
    return next == ' ' || next == '\t' || next == '\r' || next == '\n';
  }
  const std::size_t end = text.find('"', 1);
  if (end == std::string::npos) return false;
  const std::string candidate = text.substr(1, end - 1);
  if (candidate.empty()) return false;
  return detail::normalize_windows_path(candidate) ==
         detail::normalize_windows_path(exe_path);
}

// ---------------------------------------------------------------------------
// Linux: ~/.config/autostart/molink.desktop
// ---------------------------------------------------------------------------

inline const char* linux_autostart_relative_path() {
  return "autostart/molink.desktop";
}

namespace detail {

/// Desktop Entry Exec quoting. The value is always quoted, so spaces and
/// non-ASCII bytes need no special handling. Backslash and double quote are
/// escaped; `%` becomes `%%` because Exec reserves it for field codes.
inline std::string exec_quote(const std::string& path) {
  std::string out = "\"";
  for (const char ch : path) {
    if (ch == '\\' || ch == '"') {
      out.push_back('\\');
      out.push_back(ch);
    } else if (ch == '%') {
      out += "%%";
    } else {
      out.push_back(ch);
    }
  }
  out.push_back('"');
  return out;
}

}  // namespace detail

/// Complete molink.desktop contents. Written verbatim by the GTK shell.
inline std::string linux_desktop_entry(const std::string& exe_path) {
  std::string out;
  out += "[Desktop Entry]\n";
  out += "Type=Application\n";
  out += "Name=moLink\n";
  out += "Comment=moLink remote desktop\n";
  out += "Exec=" + detail::exec_quote(exe_path) + " --tray\n";
  out += "Terminal=false\n";
  out += "X-GNOME-Autostart-enabled=true\n";
  return out;
}

namespace detail {

inline std::string lower_ascii_string(std::string text) {
  for (char& ch : text) ch = lower_ascii(ch);
  return text;
}

}  // namespace detail

/// The real Linux autostart state: the file must exist and the desktop entry
/// must explicitly enable it. A missing key, `...=false`, `Hidden=true`, a
/// comment, or a key in another group all mean "not enabled".
inline bool linux_desktop_enabled(bool file_exists,
                                  const std::string& contents) {
  if (!file_exists) return false;
  // Lines before the first section header belong to the default group, which
  // is [Desktop Entry] for a .desktop file.
  bool in_desktop_entry = true;
  std::size_t start = 0;
  while (start <= contents.size()) {
    const std::size_t end = contents.find('\n', start);
    std::string line = contents.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::string trimmed = detail::trim_ascii(line);
    if (!trimmed.empty() && trimmed.front() != '#') {
      if (trimmed.front() == '[') {
        in_desktop_entry = trimmed == "[Desktop Entry]";
      } else if (in_desktop_entry) {
        const std::size_t equals = trimmed.find('=');
        if (equals != std::string::npos) {
          const std::string key =
              detail::trim_ascii(trimmed.substr(0, equals));
          const std::string value = detail::lower_ascii_string(
              detail::trim_ascii(trimmed.substr(equals + 1)));
          if (key == "X-GNOME-Autostart-enabled") {
            return value == "true";
          }
          if (key == "Hidden" && value == "true") return false;
        }
      }
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Shell preference file (Linux) and shared key/value rules
// ---------------------------------------------------------------------------

/// Only the fields the shell itself owns live here. `close_to_tray_set`
/// distinguishes "never changed" (follow autostart) from an explicit value.
struct prefs {
  bool close_to_tray_set = false;
  bool close_to_tray = false;
  bool balloon_seen = false;
};

namespace detail {

inline bool parse_bool(const std::string& raw, bool* out) {
  const std::string value = lower_ascii_string(trim_ascii(raw));
  if (value == "1" || value == "true" || value == "yes" || value == "on") {
    if (out != nullptr) *out = true;
    return true;
  }
  if (value == "0" || value == "false" || value == "no" || value == "off") {
    if (out != nullptr) *out = false;
    return true;
  }
  return false;
}

}  // namespace detail

inline prefs parse_prefs(const std::string& text) {
  prefs out;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('\n', start);
    std::string line = text.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::string trimmed = detail::trim_ascii(line);
    if (!trimmed.empty() && trimmed.front() != '#' &&
        trimmed.front() != ';') {
      const std::size_t equals = trimmed.find('=');
      if (equals != std::string::npos) {
        const std::string key =
            detail::trim_ascii(trimmed.substr(0, equals));
        const std::string value = trimmed.substr(equals + 1);
        bool parsed = false;
        if (detail::parse_bool(value, &parsed)) {
          if (key == "close_to_tray") {
            out.close_to_tray_set = true;
            out.close_to_tray = parsed;
          } else if (key == "tray_balloon_seen") {
            out.balloon_seen = parsed;
          }
        }
      }
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return out;
}

inline std::string serialize_prefs(const prefs& value) {
  std::string out;
  if (value.close_to_tray_set) {
    out += "close_to_tray=";
    out += value.close_to_tray ? "1" : "0";
    out += "\n";
  }
  if (value.balloon_seen) out += "tray_balloon_seen=1\n";
  return out;
}

inline void set_close_to_tray(prefs& value, bool close_to_tray) {
  value.close_to_tray_set = true;
  value.close_to_tray = close_to_tray;
}

inline void mark_balloon_seen(prefs& value) { value.balloon_seen = true; }

// ---------------------------------------------------------------------------
// Log line
// ---------------------------------------------------------------------------

/// `shell.tray action=hide autostart=1 close_to_tray=1`
inline std::string log_line(const char* action, bool autostart,
                            bool close_to_tray) {
  return std::string("shell.tray action=") + action + " autostart=" +
         (autostart ? "1" : "0") + " close_to_tray=" +
         (close_to_tray ? "1" : "0");
}

}  // namespace molink::tray
