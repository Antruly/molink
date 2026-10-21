#include "clipboard/clipboard.h"

#if MOLINK_HAVE_X11

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <poll.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace molink {
namespace {

// X11 property writes are bounded by the server's maximum request size
// (typically 256 KiB); keep well under it.
constexpr std::size_t kMaxClipboardBytes = 192 * 1024;

class x11_clipboard final : public clipboard_service {
 public:
  static std::unique_ptr<x11_clipboard> create(std::string& err) {
    std::unique_ptr<x11_clipboard> service(new x11_clipboard());
    if (!service->init(err)) return nullptr;
    return service;
  }

  ~x11_clipboard() override { stop(); }

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
        finish_get(std::string());
      }
    }
  }

  void pump_commands() {
    std::deque<std::string> sets;
    std::deque<text_callback> gets;
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      sets.swap(pending_sets_);
      gets.swap(pending_gets_);
    }

    for (std::string& text : sets) take_ownership(std::move(text));

    for (text_callback& callback : gets) {
      if (getting_) {
        // One transfer at a time; retry on the next tick.
        std::lock_guard<std::mutex> lock(queue_mutex_);
        pending_gets_.push_front(std::move(callback));
        break;
      }
      start_get(std::move(callback));
    }
  }

  void take_ownership(std::string text) {
    XSetSelectionOwner(display_, clipboard_, window_, CurrentTime);
    XFlush(display_);
    owns_ = XGetSelectionOwner(display_, clipboard_) == window_;
    owned_text_ = owns_ ? std::move(text) : std::string();
  }

  void start_get(text_callback callback) {
    if (XGetSelectionOwner(display_, clipboard_) == None) {
      // No owner: deliver an empty result without a round trip.
      if (callback) callback(std::string());
      return;
    }
    getting_ = true;
    incremental_ = false;
    get_buffer_.clear();
    get_callback_ = std::move(callback);
    get_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    XDeleteProperty(display_, window_, prop_);
    XConvertSelection(display_, clipboard_, utf8_, prop_, window_, CurrentTime);
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
      if (request.target == targets_) {
        const Atom supported[] = {targets_, utf8_, text_, XA_STRING};
        XChangeProperty(display_, request.requestor, property, XA_ATOM, 32,
                        PropModeReplace,
                        reinterpret_cast<const unsigned char*>(supported), 4);
        notify.property = property;
      } else if (request.target == utf8_ || request.target == text_ ||
                 request.target == XA_STRING) {
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
      finish_get(std::string());
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
      finish_get(std::string());
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
        finish_get(std::move(get_buffer_));
      } else {
        finish_get(std::string());
      }
      return;
    }

    if (get_buffer_.size() + items <= kMaxClipboardBytes) {
      get_buffer_.append(reinterpret_cast<const char*>(data), items);
    }
    XFree(data);
    XDeleteProperty(display_, window_, property);

    if (!incremental_) finish_get(std::move(get_buffer_));
  }

  Display* display_ = nullptr;
  Window window_ = 0;
  Atom clipboard_ = None;
  Atom targets_ = None;
  Atom utf8_ = None;
  Atom text_ = None;
  Atom incr_ = None;
  Atom prop_ = None;

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> available_{false};

  std::mutex queue_mutex_;
  std::deque<std::string> pending_sets_;
  std::deque<text_callback> pending_gets_;

  std::string owned_text_;
  bool owns_ = false;

  bool getting_ = false;
  bool incremental_ = false;
  std::string get_buffer_;
  std::chrono::steady_clock::time_point get_deadline_{};
  text_callback get_callback_;
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
