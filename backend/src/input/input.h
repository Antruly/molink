#pragma once

#include <memory>
#include <string>

namespace molink {

/// Injects pointer and keyboard events into the local display.
///
/// Coordinates are normalized to [0, 1] so the client does not need to know the
/// capture resolution. The interface is transport-agnostic: the WebSocket
/// handler parses JSON and calls these methods.
class input_injector {
 public:
  enum class status { ok, rejected, unavailable };

  virtual ~input_injector() = default;

  virtual bool available() const = 0;
  virtual std::string backend() const = 0;

  /// Tells the injector which monitor the remote view shows. Coordinates are
  /// in the platform's input units (desktop pixels on X11/Windows, points on
  /// macOS). The default is the whole screen.
  virtual void set_display_rect(int x, int y, int width, int height) {
    (void)x;
    (void)y;
    (void)width;
    (void)height;
  }

  virtual status move(double nx, double ny) = 0;
  virtual status button(int button, bool down) = 0;
  /// dx/dy in wheel notches; positive y scrolls down.
  virtual status wheel(int dx, int dy) = 0;
  /// @p code is a browser KeyboardEvent.code ("KeyA", "Enter", "ArrowLeft").
  virtual status key(const std::string& code, bool down) = 0;
  /// Types composed/IME text as Unicode. Used by the mobile virtual keyboard
  /// where key codes are not available.
  virtual status text(const std::string& utf8) = 0;

  /// Releases anything held down, so a client disconnect cannot leave the
  /// remote machine with a stuck modifier or button.
  virtual void release_all() = 0;
};

/// Returns nullptr when the build has no X11 support or the X server lacks
/// XTEST. Never throws.
std::unique_ptr<input_injector> make_x11_input(std::string& err);

/// Windows SendInput backend. Returns nullptr on other platforms.
std::unique_ptr<input_injector> make_windows_input(std::string& err);

/// macOS CGEvent backend. Returns nullptr on other platforms.
std::unique_ptr<input_injector> make_macos_input(std::string& err);

}  // namespace molink
