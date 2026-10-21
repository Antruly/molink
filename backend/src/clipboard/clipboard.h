#pragma once

#include <functional>
#include <memory>
#include <string>

namespace molink {

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
};

/// nullptr when the build has no X11 support or no display is reachable.
std::unique_ptr<clipboard_service> make_x11_clipboard(std::string& err);

/// Windows clipboard backend. Returns nullptr on other platforms.
std::unique_ptr<clipboard_service> make_windows_clipboard(std::string& err);

/// macOS NSPasteboard backend. Returns nullptr on other platforms.
std::unique_ptr<clipboard_service> make_macos_clipboard(std::string& err);

}  // namespace molink
