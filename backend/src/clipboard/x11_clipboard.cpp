#include "clipboard/clipboard.h"
#include "clipboard/clipboard_image.h"

#if MOLINK_HAVE_X11

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <future>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "clipboard/clipboard_files.h"
#include "diag_log.h"

namespace molink {
namespace {

// X11 property writes are bounded by the server's maximum request size
// (typically 256 KiB); keep well under it.
constexpr std::size_t kMaxClipboardBytes = 192 * 1024;

// XFixes selection tracking. The extension is in libXfixes at runtime on
// every desktop that has a clipboard manager, but the development header is
// not always installed, so the ABI is spelled out here and the library is
// loaded the same way the X11 capture backend loads XDamage. Without it the
// watcher falls back to "the selection owner changed", which misses a second
// copy of the same text from the same application.
struct xfixes_selection_notify_event {
  int type;
  unsigned long serial;
  Bool send_event;
  Display* display;
  Window window;
  int subtype;
  Window owner;
  Atom selection;
  Time timestamp;
  Time selection_timestamp;
};
constexpr int kXfixesSelectionNotify = 0;
constexpr int kXfixesSetSelectionOwnerNotify = 0;
constexpr unsigned long kXfixesSetSelectionOwnerMask = 1UL << 0;
using xfixes_query_extension_fn = Bool (*)(Display*, int*, int*);
using xfixes_select_selection_input_fn = void (*)(Display*, Window, Atom,
                                                  unsigned long);

std::string uri_decode(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size()) {
      const int hi = std::isxdigit(static_cast<unsigned char>(value[i + 1]))
                         ? std::stoi(value.substr(i + 1, 1), nullptr, 16)
                         : -1;
      const int lo = std::isxdigit(static_cast<unsigned char>(value[i + 2]))
                         ? std::stoi(value.substr(i + 2, 1), nullptr, 16)
                         : -1;
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(value[i]);
  }
  return out;
}

std::string uri_encode_path(const std::string& path) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out = "file://";
  out.reserve(path.size() + 8);
  for (const unsigned char c : path) {
    if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' ||
        c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0f]);
    }
  }
  return out;
}

/// Parse text/uri-list (one URI per line) or x-special/gnome-copied-files
/// (a copy/cut line followed by URIs). Local paths are walked, so a copied
/// folder keeps its directory entries (including empty ones) and each
/// descendant's relative path instead of arriving as loose files.
std::vector<clipboard_file> parse_uri_list(const std::string& data,
                                           bool gnome_format) {
  std::vector<std::string> roots;
  std::istringstream stream(data);
  std::string line;
  bool first_line = true;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line.front() == '#') continue;
    if (gnome_format && first_line &&
        (line == "copy" || line == "cut" || line == "copy\n")) {
      first_line = false;
      continue;
    }
    first_line = false;
    if (line.rfind("file://", 0) != 0) continue;
    const std::string rest = line.substr(7);
    const std::size_t slash = rest.find('/');
    if (slash == std::string::npos) continue;
    const std::string host = rest.substr(0, slash);
    if (!host.empty() && host != "localhost") continue;
    const std::string path = uri_decode(rest.substr(slash));
    if (path.empty()) continue;
    roots.push_back(path);
  }
  const clipboard_scan_result scan = scan_clipboard_paths(roots);
  if (scan.truncated) {
    diag().log(diag_level::warn, "clipboard.files",
               "clipboard file enumeration truncated",
               {{"truncated", "1"},
                {"omitted", std::to_string(scan.omitted)},
                {"max_files", std::to_string(kClipboardPathScanMax)},
                {"directories", std::to_string(scan.directories)}});
  }
  clipboard_path_build built = build_clipboard_files(scan.entries);
  return std::move(built.files);
}

class x11_clipboard final : public clipboard_service {
 public:
  static std::unique_ptr<x11_clipboard> create(std::string& err) {
    std::unique_ptr<x11_clipboard> service(new x11_clipboard());
    if (!service->init(err)) return nullptr;
    return service;
  }

  ~x11_clipboard() override {
    watch_running_.store(false);
    if (watch_thread_.joinable()) watch_thread_.join();
    stop();
  }

  bool available() const override { return available_.load(); }
  std::string backend() const override { return "x11-selection"; }

  void set_text(std::string text) override {
    if (text.size() > kMaxClipboardBytes) text.resize(kMaxClipboardBytes);
    std::lock_guard<std::mutex> lock(queue_mutex_);
    pending_sets_.push_back(std::move(text));
    wake();
  }

  void request_text(text_callback callback) override {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    pending_gets_.push_back(std::move(callback));
    wake();
  }

  void request_files(files_callback callback) override {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    pending_file_gets_.push_back(std::move(callback));
    wake();
  }

  void request_image(image_callback callback) override {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    pending_image_gets_.push_back(std::move(callback));
    wake();
  }

  /// Own CLIPBOARD for a PNG image. When `text` is non-empty the same owner
  /// also serves UTF8_STRING, so pasting into an editor still finds the text
  /// that was copied together with the picture.
  bool set_image(std::vector<uint8_t> png, std::string text) override {
    if (png.empty() || png.size() > kClipboardImageMaxBytes) return false;
    if (text.size() > kMaxClipboardBytes) text.resize(kMaxClipboardBytes);
    std::lock_guard<std::mutex> lock(queue_mutex_);
    pending_image_sets_.push_back({std::move(png), std::move(text)});
    wake();
    return true;
  }

  /// X11 has no change counter that is portable enough, so the owner is asked
  /// for the text now and then. The round trip runs on this thread with a
  /// short timeout; the callback fires only when the text actually changed.
  void watch(text_watcher callback) override {
    if (callback == nullptr) return;
    {
      std::lock_guard<std::mutex> lock(watch_mutex_);
      watch_callback_ = std::move(callback);
    }
    start_watch_thread();
  }

  /// X11 has no clipboard change counter either, so the same poll asks for
  /// text/uri-list. A text copy answers with an empty list, which re-arms the
  /// signature so the next file copy is reported even if its paths match.
  void watch_files(files_watcher callback) override {
    if (callback == nullptr) return;
    {
      std::lock_guard<std::mutex> lock(watch_mutex_);
      files_callback_ = std::move(callback);
    }
    start_watch_thread();
  }

  /// X11 has no change counter, so the watcher asks for image/png only when
  /// the selection serial (XFixes, or the owner fallback) reports a new copy.
  /// Requesting an 8 MiB INCR transfer on every poll would be wasteful.
  void watch_images(image_watcher callback) override {
    if (callback == nullptr) return;
    {
      std::lock_guard<std::mutex> lock(watch_mutex_);
      image_callback_ = std::move(callback);
    }
    start_watch_thread();
  }

  void start_watch_thread() {
    if (watch_running_.exchange(true)) return;
    watch_thread_ = std::thread([this] {
      // A selection event delivered between watch() and this thread would
      // otherwise look like a fresh copy on the first poll.
      uint64_t last_serial = selection_serial_.load();
      // Ask for image/png once on the first poll and then only when a new copy
      // is reported; unlike text/uri-list this may be a multi-megabyte INCR
      // transfer.
      bool image_checked = false;
      while (watch_running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        if (!watch_running_.load()) break;
        // XFixes reports every XSetSelectionOwner, so a second copy of the
        // same text is a new event. Without XFixes the fallback is an owner
        // change; either way the content is not compared to decide whether a
        // copy happened.
        const uint64_t serial = selection_serial_.load();
        const bool new_selection = serial != last_serial;
        last_serial = serial;
        auto text_promise = std::make_shared<std::promise<std::string>>();
        std::future<std::string> text_future = text_promise->get_future();
        request_text([text_promise](std::string value) {
          try {
            text_promise->set_value(std::move(value));
          } catch (...) {
            // Already satisfied or abandoned; nothing to do.
          }
        });
        if (text_future.wait_for(std::chrono::milliseconds(400)) ==
            std::future_status::ready) {
          std::string text = text_future.get();
          if (!text.empty()) {
            std::lock_guard<std::mutex> lock(watch_mutex_);
            // Report a new selection even when the value is identical; the
            // content comparison is only the safety net for a backend without
            // XFixes where the same application kept ownership.
            if (clipboard_watcher_should_report(new_selection, text,
                                                last_text_)) {
              last_text_ = text;
              last_files_.clear();
              if (watch_callback_) {
                watch_callback_(text, owner_snapshot());
              }
            }
          }
        }
        // Text and an image can coexist on one selection (LibreOffice puts
        // both there). Report the image independently so the viewer gets both
        // formats, and never report our own write as a fresh copy.
        if (new_selection || !image_checked) {
          image_checked = true;
          auto image_promise =
              std::make_shared<std::promise<clipboard_image>>();
          std::future<clipboard_image> image_future =
              image_promise->get_future();
          request_image([image_promise](clipboard_image value) {
            try {
              image_promise->set_value(std::move(value));
            } catch (...) {
              // Already satisfied or abandoned.
            }
          });
          if (image_future.wait_for(std::chrono::milliseconds(3000)) ==
              std::future_status::ready) {
            clipboard_image image = image_future.get();
            const clipboard_owner owner = owner_snapshot();
            bool report = false;
            if (owner == clipboard_owner::self) {
              std::lock_guard<std::mutex> lock(watch_mutex_);
              if (image_callback_) {
                image_callback_(clipboard_image{}, owner);
              }
            } else if (image.too_large) {
              report = true;
            } else if (image.empty()) {
              std::lock_guard<std::mutex> lock(watch_mutex_);
              last_image_hash_.clear();
            } else {
              const std::string hash = clipboard_image_hash(image.png);
              std::lock_guard<std::mutex> lock(watch_mutex_);
              report = hash != last_image_hash_;
              if (report) last_image_hash_ = hash;
            }
            if (report) {
              std::lock_guard<std::mutex> lock(watch_mutex_);
              if (image_callback_) {
                image_callback_(std::move(image), owner);
              }
            }
          }
        }

        // The file list and the text share the one CLIPBOARD selection, but
        // asking for text/uri-list when the clipboard holds text is cheap:
        // the owner answers with no target.
        auto files_promise =
            std::make_shared<std::promise<std::vector<clipboard_file>>>();
        std::future<std::vector<clipboard_file>> files_future =
            files_promise->get_future();
        request_files([files_promise](std::vector<clipboard_file> value) {
          try {
            files_promise->set_value(std::move(value));
          } catch (...) {
            // Already satisfied or abandoned.
          }
        });
        if (files_future.wait_for(std::chrono::milliseconds(500)) !=
            std::future_status::ready) {
          continue;
        }
        std::vector<clipboard_file> files = files_future.get();
        std::string signature;
        for (const clipboard_file& file : files) {
          signature += file.path;
          signature += '\n';
        }
        if (files.empty()) {
          std::lock_guard<std::mutex> lock(watch_mutex_);
          last_files_.clear();
          continue;
        }
        std::lock_guard<std::mutex> lock(watch_mutex_);
        // A new selection means a new copy even if the list is unchanged; a
        // different list is the fallback when no XFixes event was delivered.
        if (!new_selection && signature == last_files_) continue;
        last_files_ = signature;
        last_text_.clear();
        if (files_callback_) {
          files_callback_(std::move(files), owner_snapshot());
        }
      }
    });
  }

  /// Who owns CLIPBOARD right now. Xlib is not thread-safe, so the event-loop
  /// thread publishes the answer and the watcher only reads this enum. A value
  /// of `self` still reaches the callback once: the supervisor logs it and
  /// drops it, which is what makes "why was this not sent?" answerable.
  clipboard_owner owner_snapshot() const {
    switch (owner_state_.load(std::memory_order_relaxed)) {
      case 1:
        return clipboard_owner::self;
      case 2:
        return clipboard_owner::external;
      default:
        return clipboard_owner::unknown;
    }
  }

  /// Load libXfixes and subscribe to CLIPBOARD ownership changes. Optional:
  /// the watcher keeps the owner-change fallback when the library or the
  /// extension is missing.
  void init_xfixes() {
    void* lib = dlopen("libXfixes.so.3", RTLD_NOW | RTLD_LOCAL);
    if (lib == nullptr) lib = dlopen("libXfixes.so", RTLD_NOW | RTLD_LOCAL);
    if (lib == nullptr) return;
    auto query = reinterpret_cast<xfixes_query_extension_fn>(
        dlsym(lib, "XFixesQueryExtension"));
    auto select = reinterpret_cast<xfixes_select_selection_input_fn>(
        dlsym(lib, "XFixesSelectSelectionInput"));
    if (query == nullptr || select == nullptr) {
      dlclose(lib);
      return;
    }
    int event_base = 0;
    int error_base = 0;
    if (!query(display_, &event_base, &error_base)) {
      dlclose(lib);
      return;
    }
    select(display_, window_, clipboard_, kXfixesSetSelectionOwnerMask);
    XFlush(display_);
    xfixes_lib_ = lib;
    xfixes_event_base_ = event_base;
  }

  void update_owner_state() {
    const Window owner = XGetSelectionOwner(display_, clipboard_);
    int state = 0;
    if (owner == window_) {
      state = 1;
    } else if (owner != None) {
      state = 2;
    }
    owner_state_.store(state, std::memory_order_relaxed);
    // Without XFixes the only portable "a copy happened" signal is that the
    // selection owner changed. It misses a second copy from the same
    // application, which is why XFixes is preferred when present.
    if (xfixes_event_base_ < 0 && owner != fallback_owner_) {
      fallback_owner_ = owner;
      selection_serial_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  bool set_files(const std::vector<std::string>& paths) override {
    if (paths.empty()) return false;
    std::lock_guard<std::mutex> lock(queue_mutex_);
    pending_file_sets_.push_back(paths);
    wake();
    return true;
  }

 private:
  x11_clipboard() = default;

  bool init(std::string& err) {
    const char* requested = std::getenv("MOLINK_DISPLAY");
    display_ = XOpenDisplay(requested);
    if (display_ == nullptr) {
      err = "cannot open an X display for clipboard access";
      return false;
    }
    // Xlib's default error handler exits the process. Selection traffic from
    // other clients must never be able to kill the core.
    XSetErrorHandler(&ignore_error);
    window_ = XCreateSimpleWindow(display_, DefaultRootWindow(display_), -10,
                                  -10, 1, 1, 0, 0, 0);
    XSelectInput(display_, window_, PropertyChangeMask);
    clipboard_ = XInternAtom(display_, "CLIPBOARD", False);
    targets_ = XInternAtom(display_, "TARGETS", False);
    utf8_ = XInternAtom(display_, "UTF8_STRING", False);
    text_ = XInternAtom(display_, "TEXT", False);
    incr_ = XInternAtom(display_, "INCR", False);
    prop_ = XInternAtom(display_, "MOLINK_CLIPBOARD", False);
    uri_list_ = XInternAtom(display_, "text/uri-list", False);
    gnome_copied_ =
        XInternAtom(display_, "x-special/gnome-copied-files", False);
    png_ = XInternAtom(display_, "image/png", False);
    init_xfixes();

    // Clipboard commands are queued from other threads. Without a wakeup the
    // service loop would only notice them when its 100 ms poll timed out,
    // which added ~80 ms to every clipboard read (and to each 256 KiB
    // clipboard-pull range on a Linux host).
    if (::pipe(wake_fds_) == 0) {
      for (int fd : wake_fds_) {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
      }
    }

    running_.store(true);
    available_.store(true);
    thread_ = std::thread([this] { run(); });
    return true;
  }

  void wake() {
    if (wake_fds_[1] < 0) return;
    const char byte = 1;
    // A full pipe already means a wakeup is pending.
    (void)::write(wake_fds_[1], &byte, 1);
  }

  static int ignore_error(Display*, XErrorEvent*) { return 0; }

  void stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    available_.store(false);
    for (int& fd : wake_fds_) {
      if (fd >= 0) ::close(fd);
      fd = -1;
    }
    if (display_ != nullptr) {
      XCloseDisplay(display_);
      display_ = nullptr;
    }
    if (xfixes_lib_ != nullptr) {
      dlclose(xfixes_lib_);
      xfixes_lib_ = nullptr;
    }
  }

  void run() {
    const int fd = ConnectionNumber(display_);
    while (running_.load()) {
      pump_commands();
      update_owner_state();

      struct pollfd descriptors[2];
      descriptors[0].fd = fd;
      descriptors[0].events = POLLIN;
      descriptors[0].revents = 0;
      descriptors[1].fd = wake_fds_[0];
      descriptors[1].events = POLLIN;
      descriptors[1].revents = 0;
      ::poll(descriptors, wake_fds_[0] >= 0 ? 2 : 1, 100);
      if (wake_fds_[0] >= 0 && (descriptors[1].revents & POLLIN) != 0) {
        char drain[64];
        while (::read(wake_fds_[0], drain, sizeof(drain)) > 0) {
        }
      }

      while (XPending(display_) > 0) {
        XEvent event;
        XNextEvent(display_, &event);
        if (xfixes_event_base_ >= 0 &&
            event.type == xfixes_event_base_ + kXfixesSelectionNotify) {
          const auto* selection =
              reinterpret_cast<const xfixes_selection_notify_event*>(&event);
          if (selection->selection == clipboard_ &&
              selection->subtype == kXfixesSetSelectionOwnerNotify) {
            selection_serial_.fetch_add(1, std::memory_order_relaxed);
          }
          continue;
        }
        switch (event.type) {
          case SelectionRequest:
            handle_selection_request(event.xselectionrequest);
            break;
          case SelectionNotify:
            handle_selection_notify(event.xselection);
            break;
          case PropertyNotify:
            handle_property_notify(event.xproperty);
            break;
          case SelectionClear:
            if (event.xselectionclear.selection == clipboard_) {
              owns_ = false;
              owner_state_.store(2, std::memory_order_relaxed);
            }
            break;
          default:
            break;
        }
      }

      if (getting_ && std::chrono::steady_clock::now() > get_deadline_) {
        finish_transfer(std::string());
      }
    }
  }

  void pump_commands() {
    std::deque<std::string> sets;
    std::deque<std::vector<std::string>> file_sets;
    std::deque<image_set> image_sets;
    std::deque<text_callback> gets;
    std::deque<files_callback> file_gets;
    std::deque<image_callback> image_gets;
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      sets.swap(pending_sets_);
      file_sets.swap(pending_file_sets_);
      image_sets.swap(pending_image_sets_);
      gets.swap(pending_gets_);
      file_gets.swap(pending_file_gets_);
      image_gets.swap(pending_image_gets_);
    }

    for (std::string& text : sets) take_text_ownership(std::move(text));
    for (const std::vector<std::string>& paths : file_sets) {
      take_file_ownership(paths);
    }
    for (image_set& image : image_sets) {
      take_image_ownership(std::move(image.png), std::move(image.text));
    }

    // One selection transfer at a time. When one is already running, requeue
    // *every* remaining callback, not just the first: the others used to be
    // destroyed with this local deque, so a second clipboard request in the
    // same tick (for example the text poll and the page's file poll) never
    // called back. The supervisor resets its in-flight flag inside that
    // callback, so losing it disabled clipboard sync until the core restarted.
    std::deque<text_callback> deferred_gets;
    for (text_callback& callback : gets) {
      if (getting_) {
        deferred_gets.push_back(std::move(callback));
        continue;
      }
      start_get(std::move(callback));
    }
    if (!deferred_gets.empty()) {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      while (!deferred_gets.empty()) {
        pending_gets_.push_front(std::move(deferred_gets.back()));
        deferred_gets.pop_back();
      }
    }
    std::deque<files_callback> deferred_file_gets;
    for (files_callback& callback : file_gets) {
      if (getting_) {
        deferred_file_gets.push_back(std::move(callback));
        continue;
      }
      start_file_get(std::move(callback));
    }
    if (!deferred_file_gets.empty()) {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      while (!deferred_file_gets.empty()) {
        pending_file_gets_.push_front(std::move(deferred_file_gets.back()));
        deferred_file_gets.pop_back();
      }
    }
    std::deque<image_callback> deferred_image_gets;
    for (image_callback& callback : image_gets) {
      if (getting_) {
        deferred_image_gets.push_back(std::move(callback));
        continue;
      }
      start_image_get(std::move(callback));
    }
    if (!deferred_image_gets.empty()) {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      while (!deferred_image_gets.empty()) {
        pending_image_gets_.push_front(
            std::move(deferred_image_gets.back()));
        deferred_image_gets.pop_back();
      }
    }
  }

  void claim_ownership() {
    XSetSelectionOwner(display_, clipboard_, window_, CurrentTime);
    XFlush(display_);
    owns_ = XGetSelectionOwner(display_, clipboard_) == window_;
    owner_state_.store(owns_ ? 1 : 0, std::memory_order_relaxed);
    if (!owns_) {
      owned_text_.clear();
      owned_png_.clear();
      owned_uri_list_.clear();
      owned_gnome_.clear();
    }
  }

  void take_text_ownership(std::string text) {
    owned_text_ = std::move(text);
    owned_png_.clear();
    owned_uri_list_.clear();
    owned_gnome_.clear();
    claim_ownership();
  }

  void take_image_ownership(std::vector<uint8_t> png, std::string text) {
    owned_png_ = std::move(png);
    owned_text_ = std::move(text);
    owned_uri_list_.clear();
    owned_gnome_.clear();
    claim_ownership();
  }

  void take_file_ownership(const std::vector<std::string>& paths) {
    owned_text_.clear();
    owned_png_.clear();
    owned_uri_list_.clear();
    owned_gnome_.clear();
    std::string list;
    for (const std::string& path : paths) {
      if (path.empty()) continue;
      const std::string uri = uri_encode_path(path);
      if (!list.empty()) list += "\r\n";
      list += uri;
    }
    if (list.empty()) return;
    owned_uri_list_ = list + "\r\n";
    owned_gnome_ = "copy\n" + list + "\n";
    claim_ownership();
  }

  void start_get(text_callback callback) {
    if (XGetSelectionOwner(display_, clipboard_) == None) {
      // No owner: deliver an empty result without a round trip.
      if (callback) callback(std::string());
      return;
    }
    getting_ = true;
    getting_files_ = false;
    getting_image_ = false;
    transfer_max_bytes_ = kMaxClipboardBytes;
    transfer_total_bytes_ = 0;
    transfer_overflow_ = false;
    incremental_ = false;
    get_buffer_.clear();
    get_callback_ = std::move(callback);
    file_get_callback_ = nullptr;
    image_get_callback_ = nullptr;
    get_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    XDeleteProperty(display_, window_, prop_);
    XConvertSelection(display_, clipboard_, utf8_, prop_, window_, CurrentTime);
    XFlush(display_);
  }

  void start_file_get(files_callback callback) {
    if (XGetSelectionOwner(display_, clipboard_) == None) {
      if (callback) callback({});
      return;
    }
    getting_ = true;
    getting_files_ = true;
    getting_image_ = false;
    transfer_max_bytes_ = kMaxClipboardBytes;
    transfer_total_bytes_ = 0;
    transfer_overflow_ = false;
    file_stage_ = 0;
    incremental_ = false;
    get_buffer_.clear();
    get_callback_ = nullptr;
    file_get_callback_ = std::move(callback);
    image_get_callback_ = nullptr;
    get_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    XDeleteProperty(display_, window_, prop_);
    XConvertSelection(display_, clipboard_, uri_list_, prop_, window_,
                      CurrentTime);
    XFlush(display_);
  }

  void start_image_get(image_callback callback) {
    if (XGetSelectionOwner(display_, clipboard_) == None) {
      if (callback) callback(clipboard_image{});
      return;
    }
    getting_ = true;
    getting_files_ = false;
    getting_image_ = true;
    transfer_max_bytes_ = kClipboardImageMaxBytes;
    transfer_total_bytes_ = 0;
    transfer_overflow_ = false;
    incremental_ = false;
    get_buffer_.clear();
    get_callback_ = nullptr;
    file_get_callback_ = nullptr;
    image_get_callback_ = std::move(callback);
    // An INCR screenshot is several megabytes; the owner is a local
    // application, so allow more time than a text copy.
    get_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    XDeleteProperty(display_, window_, prop_);
    XConvertSelection(display_, clipboard_, png_, prop_, window_, CurrentTime);
    XFlush(display_);
  }

  void finish_get(std::string text) {
    getting_ = false;
    incremental_ = false;
    get_buffer_.clear();
    text_callback callback = std::move(get_callback_);
    get_callback_ = nullptr;
    if (callback) callback(std::move(text));
  }

  /// Finish a file-list transfer. If the owner only exports the GNOME
  /// format, retry once with that target before delivering an empty list.
  void finish_file_get() {
    getting_ = false;
    incremental_ = false;
    const std::string data = std::move(get_buffer_);
    get_buffer_.clear();
    std::vector<clipboard_file> files =
        parse_uri_list(data, file_stage_ == 1);
    if (files.empty() && file_stage_ == 0) {
      file_stage_ = 1;
      getting_ = true;
      transfer_total_bytes_ = 0;
      transfer_overflow_ = false;
      incremental_ = false;
      get_deadline_ = std::chrono::steady_clock::now() +
                      std::chrono::seconds(2);
      XDeleteProperty(display_, window_, prop_);
      XConvertSelection(display_, clipboard_, gnome_copied_, prop_, window_,
                        CurrentTime);
      XFlush(display_);
      return;
    }
    getting_files_ = false;
    files_callback callback = std::move(file_get_callback_);
    file_get_callback_ = nullptr;
    if (callback) callback(std::move(files));
  }

  void finish_image_get() {
    getting_ = false;
    getting_image_ = false;
    incremental_ = false;
    const std::string data = std::move(get_buffer_);
    get_buffer_.clear();
    image_callback callback = std::move(image_get_callback_);
    image_get_callback_ = nullptr;
    if (!callback) return;
    if (transfer_overflow_) {
      // The PNG is larger than the 8 MiB cap. Keep the size (a lower bound is
      // still useful in the diagnostic) but never hand a truncated image on.
      clipboard_image image;
      image.too_large = true;
      image.source_bytes = transfer_total_bytes_;
      callback(std::move(image));
      return;
    }
    std::vector<uint8_t> png(data.begin(), data.end());
    callback(clipboard_image_clamped(std::move(png)));
  }

  /// Common end-of-transfer hook: text, file lists and images share the
  /// buffer and the INCR machinery.
  void finish_transfer(const std::string& text) {
    if (getting_image_) {
      if (!text.empty() && get_buffer_.empty()) get_buffer_ = text;
      finish_image_get();
    } else if (getting_files_) {
      if (!text.empty()) get_buffer_ = text;
      finish_file_get();
    } else {
      finish_get(text);
    }
  }

  void handle_selection_request(XSelectionRequestEvent& request) {
    XSelectionEvent notify;
    std::memset(&notify, 0, sizeof(notify));
    notify.type = SelectionNotify;
    notify.display = request.display;
    notify.requestor = request.requestor;
    notify.selection = request.selection;
    notify.target = request.target;
    notify.time = request.time;
    notify.property = None;

    if (request.selection == clipboard_ && owns_) {
      const Atom property =
          request.property != None ? request.property : request.target;
      const bool has_files = !owned_uri_list_.empty();
      if (request.target == targets_) {
        // An image and its text are published together; TARGETS lists both so
        // an editor still finds UTF8_STRING and a viewer finds image/png.
        Atom supported[5];
        int count = 0;
        supported[count++] = targets_;
        if (has_files) {
          supported[count++] = uri_list_;
          supported[count++] = gnome_copied_;
        } else {
          supported[count++] = utf8_;
          supported[count++] = text_;
          supported[count++] = XA_STRING;
          if (!owned_png_.empty()) supported[count++] = png_;
        }
        XChangeProperty(display_, request.requestor, property, XA_ATOM, 32,
                        PropModeReplace,
                        reinterpret_cast<const unsigned char*>(supported),
                        count);
        notify.property = property;
      } else if (has_files &&
                 (request.target == uri_list_ ||
                  request.target == gnome_copied_)) {
        const std::string& payload = request.target == uri_list_
                                         ? owned_uri_list_
                                         : owned_gnome_;
        XChangeProperty(
            display_, request.requestor, property, request.target, 8,
            PropModeReplace,
            reinterpret_cast<const unsigned char*>(payload.data()),
            static_cast<int>(payload.size()));
        notify.property = property;
      } else if (!has_files && request.target == png_ &&
                 !owned_png_.empty()) {
        XChangeProperty(
            display_, request.requestor, property, png_, 8, PropModeReplace,
            reinterpret_cast<const unsigned char*>(owned_png_.data()),
            static_cast<int>(owned_png_.size()));
        notify.property = property;
      } else if (!has_files && !owned_text_.empty() &&
                 (request.target == utf8_ || request.target == text_ ||
                  request.target == XA_STRING)) {
        const Atom type =
            request.target == XA_STRING ? XA_STRING : utf8_;
        XChangeProperty(
            display_, request.requestor, property, type, 8, PropModeReplace,
            reinterpret_cast<const unsigned char*>(owned_text_.data()),
            static_cast<int>(owned_text_.size()));
        notify.property = property;
      }
    }
    XSendEvent(display_, request.requestor, False, NoEventMask,
               reinterpret_cast<XEvent*>(&notify));
    XFlush(display_);
  }

  void handle_selection_notify(XSelectionEvent& event) {
    if (!getting_ || event.selection != clipboard_) return;
    if (event.property == None) {
      finish_transfer(std::string());
      return;
    }
    read_property(event.property, true);
  }

  void handle_property_notify(XPropertyEvent& event) {
    if (!getting_ || !incremental_ || event.window != window_ ||
        event.atom != prop_ || event.state != PropertyNewValue) {
      return;
    }
    // A zero-length chunk terminates an INCR transfer.
    read_property(prop_, false);
  }

  void read_property(Atom property, bool first) {
    Atom type = None;
    int format = 0;
    unsigned long items = 0;
    unsigned long after = 0;
    unsigned char* data = nullptr;
    const int status =
        XGetWindowProperty(display_, window_, property, 0, 0x1fffffff, False,
                           AnyPropertyType, &type, &format, &items, &after,
                           &data);
    if (status != Success) {
      finish_transfer(std::string());
      return;
    }

    if (first && type == incr_) {
      incremental_ = true;
      if (data != nullptr) XFree(data);
      XDeleteProperty(display_, window_, property);
      // Keep the transfer going: property notifications follow.
      return;
    }

    if (items == 0 || format != 8) {
      if (data != nullptr) XFree(data);
      XDeleteProperty(display_, window_, property);
      if (incremental_) {
        finish_transfer(get_buffer_);
      } else {
        finish_transfer(std::string());
      }
      return;
    }

    transfer_total_bytes_ += items;
    if (get_buffer_.size() + items <= transfer_max_bytes_) {
      get_buffer_.append(reinterpret_cast<const char*>(data), items);
    } else {
      // Keep draining an INCR transfer so the owner is not left blocked, but
      // remember that the value did not fit and must be reported as too large.
      transfer_overflow_ = true;
    }
    XFree(data);
    XDeleteProperty(display_, window_, property);

    if (!incremental_) finish_transfer(get_buffer_);
  }

  Display* display_ = nullptr;
  Window window_ = 0;
  Atom clipboard_ = None;
  Atom targets_ = None;
  Atom utf8_ = None;
  Atom text_ = None;
  Atom incr_ = None;
  Atom prop_ = None;
  Atom uri_list_ = None;
  Atom gnome_copied_ = None;
  Atom png_ = None;

  struct image_set {
    std::vector<uint8_t> png;
    std::string text;
  };

  /// XFixes (loaded at runtime): each XSetSelectionOwner is one event, which
  /// is what makes "copy the same text again" visible. -1 when unavailable.
  void* xfixes_lib_ = nullptr;
  int xfixes_event_base_ = -1;
  std::atomic<uint64_t> selection_serial_{0};
  /// Fallback when XFixes is missing: the owner window changed. Only touched
  /// on the service thread.
  Window fallback_owner_ = None;

  std::thread thread_;
  /// Self-pipe used to interrupt the poll when another thread queues work.
  int wake_fds_[2] = {-1, -1};
  std::mutex watch_mutex_;
  text_watcher watch_callback_;
  files_watcher files_callback_;
  image_watcher image_callback_;
  std::string last_text_;
  std::string last_files_;
  std::string last_image_hash_;
  std::atomic<bool> watch_running_{false};
  std::thread watch_thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> available_{false};

  std::mutex queue_mutex_;
  std::deque<std::string> pending_sets_;
  std::deque<std::vector<std::string>> pending_file_sets_;
  std::deque<image_set> pending_image_sets_;
  std::deque<text_callback> pending_gets_;
  std::deque<files_callback> pending_file_gets_;
  std::deque<image_callback> pending_image_gets_;

  std::string owned_text_;
  std::vector<uint8_t> owned_png_;
  std::string owned_uri_list_;
  std::string owned_gnome_;
  bool owns_ = false;
  /// 0 = no owner/unknown, 1 = this process, 2 = another client. Published by
  /// the event loop (Xlib is not thread-safe) for the watcher thread.
  std::atomic<int> owner_state_{0};

  bool getting_ = false;
  bool getting_files_ = false;
  bool getting_image_ = false;
  int file_stage_ = 0;  // 0 = text/uri-list, 1 = gnome-copied-files
  bool incremental_ = false;
  /// Buffer ceiling for the current transfer: 192 KiB for text/file lists,
  /// 8 MiB for PNG. `transfer_total_bytes_` keeps counting past the ceiling so
  /// an oversized image is reported with its real size.
  std::size_t transfer_max_bytes_ = kMaxClipboardBytes;
  uint64_t transfer_total_bytes_ = 0;
  bool transfer_overflow_ = false;
  std::string get_buffer_;
  std::chrono::steady_clock::time_point get_deadline_{};
  text_callback get_callback_;
  files_callback file_get_callback_;
  image_callback image_get_callback_;
};

}  // namespace

std::unique_ptr<clipboard_service> make_x11_clipboard(std::string& err) {
  return x11_clipboard::create(err);
}

}  // namespace molink

#else  // !MOLINK_HAVE_X11

namespace molink {
std::unique_ptr<clipboard_service> make_x11_clipboard(std::string& err) {
  err = "built without X11 clipboard support";
  return nullptr;
}
}  // namespace molink

#endif
