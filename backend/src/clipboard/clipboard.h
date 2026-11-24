#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "clipboard/clipboard_route.h"

namespace molink {

class lazy_clipboard;

/// One entry of the clipboard's file list (CF_HDROP on Windows, text/uri-list
/// on X11). A copied folder keeps its directory entries and the relative path
/// of every descendant, so the viewer can recreate the tree instead of
/// flattening it.
struct clipboard_file {
  /// Display name. Always the last path component, never a relative path; the
  /// older wire field keeps that meaning and old peers stay usable.
  std::string name;
  std::string path;
  uint64_t size = 0;
  /// Non-zero when the bytes live behind an OLE data object instead of a path
  /// on this machine (an inner RDP session's virtual file). `path` is empty in
  /// that case and `read_file_range` answers the ranges. Opaque to callers.
  uint64_t stream_id = 0;
  /// A directory entry. It has no bytes; `size` is zero and the viewer creates
  /// the directory (including an empty one) instead of pulling a file.
  bool is_dir = false;
  /// Path relative to the copied selection, `/`-separated: `sub/dir/file` for
  /// a descendant, the base name for a top-level item. Empty means `name`.
  std::string rel;
};

/// True when the entry can be read at all. A virtual entry has no path but a
/// stream id; a real entry has a path. Directories are announced without
/// bytes but are still valid clipboard entries.
inline bool clipboard_file_is_announceable(const clipboard_file& file) {
  return (!file.path.empty() || file.stream_id != 0 || file.is_dir) &&
         !file.name.empty();
}

/// True when the entry names bytes that can be pulled.
inline bool clipboard_file_has_bytes(const clipboard_file& file) {
  return !file.is_dir && (!file.path.empty() || file.stream_id != 0);
}

/// The relative path the wire should carry: `rel` when set, otherwise the
/// display name.
inline std::string clipboard_file_relative(const clipboard_file& file) {
  return file.rel.empty() ? file.name : file.rel;
}

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
  /// changes. Used to push what the host user copied to the active viewer,
  /// which is what makes the clipboard behave like RDP's redirection in both
  /// directions. Implementations poll their platform's change counter.
  ///
  /// `owner` says who set the value. A change this process made itself is
  /// still reported (once) so the caller can log `clipboard.owner
  /// self=1 source=self` and drop it; everything else, including `unknown`, is
  /// a normal local copy and must be accepted.
  using text_watcher = std::function<void(std::string, clipboard_owner)>;
  virtual void watch(text_watcher callback) { (void)callback; }

  /// Called when the local clipboard's *file list* changes (Explorer Ctrl+C).
  /// Viewers show the files the host user copied without a button press.
  using files_watcher =
      std::function<void(std::vector<clipboard_file>, clipboard_owner)>;
  virtual void watch_files(files_watcher callback) { (void)callback; }

  using files_callback = std::function<void(std::vector<clipboard_file>)>;
  virtual void request_files(files_callback callback) {
    if (callback) callback({});
  }
  virtual bool set_files(const std::vector<std::string>&) { return false; }

  /// Result of one virtual-file range read. `status` uses the clipboard-pull
  /// wire codes: 0 ok, 1 read error, 2 clipboard changed / entry gone.
  using file_range_callback = std::function<void(
      uint32_t status, uint64_t offset, std::vector<uint8_t> data)>;

  /// Reads a byte range of an entry whose `stream_id` is non-zero.
  ///
  /// The bytes may only be obtainable through an RDP channel, and
  /// `IStream::Read` can block, so the backend runs the read on its own worker
  /// thread and invokes `callback` there (or on its timeout thread). Callers
  /// must hop back to their own loop before touching app state. A backend that
  /// does not implement virtual files reports `gone` immediately.
  virtual void read_file_range(const clipboard_file&, uint64_t offset,
                               uint64_t, file_range_callback callback) {
    if (callback) callback(2, offset, {});
  }

  /// Delayed rendering: the host becomes the clipboard owner but sends no
  /// file bytes until a remote application pastes. Only platforms with a
  /// delayed-render clipboard implement this; everyone else keeps the
  /// upload-then-set path.
  virtual bool supports_lazy_files() const { return false; }

  /// Takes ownership of the clipboard for the offer `lazy` currently holds.
  /// The backend keeps the registry and asks it for the descriptors when the
  /// OS renders. Returns false when this backend cannot delay file formats.
  virtual bool install_lazy(std::shared_ptr<lazy_clipboard> lazy,
                            std::string& error) {
    (void)lazy;
    error = "delayed file clipboard rendering is unavailable";
    return false;
  }

  /// Releases the delayed-render ownership. The current offer must already
  /// have been cleared (its source failed) or the waiter may block for the
  /// render timeout.
  virtual void uninstall_lazy() {}

  /// True while this backend owns the clipboard for a lazy offer and is
  /// serving descriptors instead of real paths.
  virtual bool lazy_rendering() const { return false; }

  /// True when the current clipboard is owned by an inner RDP client (the
  /// host is running mstsc and the user is working inside the nested session).
  /// This is informational (and lets the viewer show an "inner RDP" badge); it
  /// no longer disables delayed/virtual files. Reading a file copied inside
  /// the nested session now goes through the OLE data object's
  /// FileGroupDescriptorW/FileContents formats, so no host-side copy is made.
  virtual bool nested_rdp() const { return false; }
};

/// nullptr when the build has no X11 support or no display is reachable.
std::unique_ptr<clipboard_service> make_x11_clipboard(std::string& err);

/// Windows clipboard backend. Returns nullptr on other platforms.
std::unique_ptr<clipboard_service> make_windows_clipboard(std::string& err);

/// macOS NSPasteboard backend. Returns nullptr on other platforms.
std::unique_ptr<clipboard_service> make_macos_clipboard(std::string& err);

}  // namespace molink
