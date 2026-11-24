#pragma once

#include <cstddef>
#include <string>

namespace molink {

/// Which host keyboard API a code is being resolved for.
enum class keymap_platform { windows, x11, macos };

/// One resolved `KeyboardEvent.code`.
///
/// `found` says the code is part of the host's key set. A code that exists in
/// the DOM but has no physical key on this OS is `found` and `unsupported`:
/// the caller must reject it instead of pretending it is unknown. A caller
/// that only checks `found` would silently drop those, which is exactly what
/// the DOM coverage test guards against.
///
/// `scan` and `keycode` are physical identifiers: they depend on the position
/// of the key, never on the host's layout. That is what makes a client's
/// physical KeyY release the host's physical Y key on a QWERTZ or AZERTY
/// desktop instead of retyping whatever character the layout puts there.
struct key_lookup {
  bool found = false;
  bool unsupported = false;
  /// Windows virtual-key code, or macOS `CGKeyCode`.
  unsigned int value = 0;
  /// Windows set-1 scan code (without the E0 prefix). Zero means the key has
  /// no usable scan code and the injector falls back to `value` as a virtual
  /// key; only a handful of legacy/IME keys are in that group.
  unsigned int scan = 0;
  /// X11 key code. Linux evdev codes are shifted by eight into X key codes
  /// (`evdev + 8`), which is fixed by the X server's evdev keymap and is
  /// independent of the layout. Zero means the host has no physical key for
  /// this code, and the key is marked unsupported.
  unsigned int keycode = 0;
  /// X11 keysym name. Injection no longer translates through it; the
  /// `MOLINK_KEYMAP=layout` compatibility path still does, and tests use it
  /// to describe what each physical key means.
  std::string keysym;
  /// Windows: the key must carry `KEYEVENTF_EXTENDEDKEY` (E0 prefix).
  bool extended = false;
};

/// Resolve a browser `KeyboardEvent.code` (or one of the legacy aliases) for
/// one host platform. Values are the same data the production injectors use,
/// so a test can verify the whole table without a live host.
key_lookup lookup_key(keymap_platform platform, const std::string& code);

/// Whether the key's output depends on the host NumLock state. The keypad
/// digits and the decimal point are the keys Windows/X11 translate to
/// navigation keys while NumLock is off.
bool key_depends_on_numlock(const std::string& code);

/// Every `KeyboardEvent.code` the host understands, in a stable order. The
/// keymap test iterates this list once per platform and requires each entry
/// to be either mapped or explicitly unsupported.
extern const char* const kCanonicalKeyCodes[];
extern const std::size_t kCanonicalKeyCodeCount;

}  // namespace molink
