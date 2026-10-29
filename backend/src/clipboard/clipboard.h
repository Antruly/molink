#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace molink {

/// One entry of the clipboard's file list (CF_HDROP on Windows).
struct clipboard_file {
  std::string name;
  std::string path;
  uint64_t size = 0;
};

/// Text clipboard synchronization with the local display.
///
/// Requests are asynchronous because X11 selection transfer involves a
/// round-trip with the current owner. The callback runs on the clipboard
/// thread, so callers must hop back to their own loop before touching app
/// state.
class clipboard_service {
 public:
  using text_callback = std::function<void(std::string)>;

  virtual ~clipboard_service() = default;

  virtual bool available() const = 0;
  virtual std::string backend() const = 0;

  /// Take ownership of CLIPBOARD and serve this text to local applications.
  virtual void set_text(std::string text) = 0;

  /// Ask the current owner for text. Calls back with an empty string when the
  /// clipboard is empty or the owner does not answer.
  virtual void request_text(text_callback callback) = 0;

  /// File clipboard (Explorer Ctrl+C). Backends without it return an empty
  /// list and refuse set_files.
  /// Called on the backend's own thread whenever the local clipboard text
  /// changes. Used to push what the host user copied to every viewer, which is
  /// what makes the clipboard behave like RDP's redirection in both
  /// directions. Implementations poll their platform's change counter.
  using text_watcher = std::function<void(std::string)>;
  virtual void watch(text_watcher callback) { (void)callback; }

  /// Called when the local clipboard's *file list* changes (Explorer Ctrl+C).
  /// Viewers show the files the host user copied without a button press.
  using files_watcher = std::function<void(std::vector<clipboard_file>)>;
  virtual void watch_files(files_watcher callback) { (void)callback; }

  using files_callback = std::function<void(std::vector<clipboard_file>)>;
  virtual void request_files(files_callback callback) {
    if (callback) callback({});
  }
  virtual bool set_files(const std::vector<std::string>&) { return false; }
};

/// nullptr when the build has no X11 support or no display is reachable.
std::unique_ptr<clipboard_service> make_x11_clipboard(std::string& err);

/// Windows clipboard backend. Returns nullptr on other platforms.
std::unique_ptr<clipboard_service> make_windows_clipboard(std::string& err);

/// macOS NSPasteboard backend. Returns nullptr on other platforms.
std::unique_ptr<clipboard_service> make_macos_clipboard(std::string& err);

}  // namespace molink
