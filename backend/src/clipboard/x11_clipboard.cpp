#include "clipboard/clipboard.h"

#if MOLINK_HAVE_X11

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <poll.h>

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

namespace molink {
namespace {

// X11 property writes are bounded by the server's maximum request size
// (typically 256 KiB); keep well under it.
constexpr std::size_t kMaxClipboardBytes = 192 * 1024;

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
/// (a copy/cut line followed by URIs). Only local regular files are returned.
std::vector<clipboard_file> parse_uri_list(const std::string& data,
                                           bool gnome_format) {
  std::vector<clipboard_file> files;
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
    std::error_code ec;
    const std::filesystem::path fs_path(path);
    if (!std::filesystem::is_regular_file(fs_path, ec) || ec) continue;
    clipboard_file file;
    file.path = path;
    file.name = fs_path.filename().string();
    const auto size = std::filesystem::file_size(fs_path, ec);
    file.size = ec ? 0 : static_cast<uint64_t>(size);
    bool duplicate = false;
    for (const clipboard_file& existing : files) {
      if (existing.path == file.path) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) files.push_back(std::move(file));
  }
  return files;
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
  }

  void request_text(text_callback callback) override {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    pending_gets_.push_back(std::move(callback));
  }

  void request_files(files_callback callback) override {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    pending_file_gets_.push_back(std::move(callback));
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
    if (watch_running_.exchange(true)) return;
    watch_thread_ = std::thread([this] {
      while (watch_running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        if (!watch_running_.load()) break;
        auto promise = std::make_shared<std::promise<std::string>>();
        std::future<std::string> future = promise->get_future();
        request_text([promise](std::string value) {
          try {
            promise->set_value(std::move(value));
          } catch (...) {
            // Already satisfied or abandoned; nothing to do.
          }
        });
        if (future.wait_for(std::chrono::milliseconds(400)) !=
            std::future_status::ready) {
          continue;
        }
        std::string text = future.get();
        if (text.empty()) continue;
        std::lock_guard<std::mutex> lock(watch_mutex_);
        if (text == last_text_) continue;
        last_text_ = text;
        if (watch_callback_) watch_callback_(text);
      }
    });
  }

  bool set_files(const std::vector<std::string>& paths) override {
    if (paths.empty()) return false;
    std::lock_guard<std::mutex> lock(queue_mutex_);
    pending_file_sets_.push_back(paths);
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

    running_.store(true);
    available_.store(true);
    thread_ = std::thread([this] { run(); });
    return true;
  }

  static int ignore_error(Display*, XErrorEvent*) { return 0; }

  void stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    available_.store(false);
    if (display_ != nullptr) {
      XCloseDisplay(display_);
      display_ = nullptr;
    }
  }

  void run() {
    const int fd = ConnectionNumber(display_);
    while (running_.load()) {
      pump_commands();

      struct pollfd descriptor;
      descriptor.fd = fd;
      descriptor.events = POLLIN;
      descriptor.revents = 0;
      ::poll(&descriptor, 1, 100);

      while (XPending(display_) > 0) {
        XEvent event;
        XNextEvent(display_, &event);
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
            if (event.xselectionclear.selection == clipboard_) owns_ = false;
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
    std::deque<text_callback> gets;
    std::deque<files_callback> file_gets;
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      sets.swap(pending_sets_);
      file_sets.swap(pending_file_sets_);
      gets.swap(pending_gets_);
      file_gets.swap(pending_file_gets_);
    }

    for (std::string& text : sets) take_text_ownership(std::move(text));
    for (const std::vector<std::string>& paths : file_sets) {
      take_file_ownership(paths);
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
  }

  void claim_ownership() {
    XSetSelectionOwner(display_, clipboard_, window_, CurrentTime);
    XFlush(display_);
    owns_ = XGetSelectionOwner(display_, clipboard_) == window_;
    if (!owns_) {
      owned_text_.clear();
      owned_uri_list_.clear();
      owned_gnome_.clear();
    }
  }

  void take_text_ownership(std::string text) {
    owned_text_ = std::move(text);
    owned_uri_list_.clear();
    owned_gnome_.clear();
    claim_ownership();
  }

  void take_file_ownership(const std::vector<std::string>& paths) {
    owned_text_.clear();
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
    incremental_ = false;
    get_buffer_.clear();
    get_callback_ = std::move(callback);
    file_get_callback_ = nullptr;
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
    file_stage_ = 0;
    incremental_ = false;
    get_buffer_.clear();
    get_callback_ = nullptr;
    file_get_callback_ = std::move(callback);
    get_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    XDeleteProperty(display_, window_, prop_);
    XConvertSelection(display_, clipboard_, uri_list_, prop_, window_,
                      CurrentTime);
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

  /// Common end-of-transfer hook: text and file lists share the buffer and
  /// the INCR machinery.
  void finish_transfer(const std::string& text) {
    if (getting_files_) {
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
        const Atom text_targets[] = {targets_, utf8_, text_, XA_STRING};
        const Atom file_targets[] = {targets_, uri_list_, gnome_copied_};
        const Atom* supported = has_files ? file_targets : text_targets;
        const int count = has_files ? 3 : 4;
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
      } else if (!has_files &&
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

    if (get_buffer_.size() + items <= kMaxClipboardBytes) {
      get_buffer_.append(reinterpret_cast<const char*>(data), items);
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

  std::thread thread_;
  std::mutex watch_mutex_;
  text_watcher watch_callback_;
  std::string last_text_;
  std::atomic<bool> watch_running_{false};
  std::thread watch_thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> available_{false};

  std::mutex queue_mutex_;
  std::deque<std::string> pending_sets_;
  std::deque<std::vector<std::string>> pending_file_sets_;
  std::deque<text_callback> pending_gets_;
  std::deque<files_callback> pending_file_gets_;

  std::string owned_text_;
  std::string owned_uri_list_;
  std::string owned_gnome_;
  bool owns_ = false;

  bool getting_ = false;
  bool getting_files_ = false;
  int file_stage_ = 0;  // 0 = text/uri-list, 1 = gnome-copied-files
  bool incremental_ = false;
  std::string get_buffer_;
  std::chrono::steady_clock::time_point get_deadline_{};
  text_callback get_callback_;
  files_callback file_get_callback_;
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
