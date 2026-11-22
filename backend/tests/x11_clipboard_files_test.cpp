// Integration test for the X11 file clipboard writer.
//
// It takes ownership of CLIPBOARD for a moment (like the Windows
// clipboard-stream test), so it only runs with MOLINK_X11_CLIPBOARD_TEST=1.
// It then asks the selection for text/uri-list and
// x-special/gnome-copied-files exactly as a file manager would, which is the
// same check `xclip -selection clipboard -t text/uri-list -o` performs.
#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <poll.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "clipboard/clipboard.h"

namespace {

int checks = 0;
int failures = 0;

void expect(bool value, const char* message) {
  ++checks;
  if (!value) {
    ++failures;
    std::printf("FAIL: %s\n", message);
  }
}

/// Ask the current CLIPBOARD owner for `target` and return the property bytes.
std::string read_target(Display* display, Window window, Atom clipboard,
                        Atom target, Atom property, int timeout_ms = 1000) {
  XDeleteProperty(display, window, property);
  XConvertSelection(display, clipboard, target, property, window, CurrentTime);
  XFlush(display);
  const int fd = ConnectionNumber(display);
  const int attempts = timeout_ms / 100 + 1;
  for (int attempt = 0; attempt < attempts; ++attempt) {
    while (XPending(display) > 0) {
      XEvent event;
      XNextEvent(display, &event);
      if (event.type == SelectionNotify &&
          event.xselection.selection == clipboard) {
        if (event.xselection.property == None) return std::string();
        Atom type = None;
        int format = 0;
        unsigned long items = 0;
        unsigned long after = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(display, window, property, 0, 0x1fffffff, False,
                               AnyPropertyType, &type, &format, &items,
                               &after, &data) != Success) {
          return std::string();
        }
        std::string out;
        if (data != nullptr && format == 8) {
          out.assign(reinterpret_cast<const char*>(data), items);
        }
        if (data != nullptr) XFree(data);
        return out;
      }
    }
    struct pollfd descriptor;
    descriptor.fd = fd;
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    ::poll(&descriptor, 1, 100);
  }
  return std::string();
}

}  // namespace

int main() {
  if (std::getenv("MOLINK_X11_CLIPBOARD_TEST") == nullptr) {
    std::printf("SKIP: set MOLINK_X11_CLIPBOARD_TEST=1 to replace the clipboard\n");
    return 77;
  }
  Display* display = XOpenDisplay(nullptr);
  if (display == nullptr) {
    std::printf("SKIP: no X display\n");
    return 77;
  }

  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "molink-x11-clipboard-test";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  const std::filesystem::path file = dir / "fixture.txt";
  {
    std::ofstream out(file, std::ios::binary);
    out << "clipboard file";
  }

  std::string error;
  std::unique_ptr<molink::clipboard_service> service =
      molink::make_x11_clipboard(error);
  if (!service) {
    std::printf("SKIP: x11 clipboard unavailable: %s\n", error.c_str());
    return 77;
  }

  const Atom clipboard = XInternAtom(display, "CLIPBOARD", False);
  const Atom uri_list = XInternAtom(display, "text/uri-list", False);
  const Atom gnome =
      XInternAtom(display, "x-special/gnome-copied-files", False);
  const Atom property = XInternAtom(display, "MOLINK_TEST_SELECTION", False);
  const Window window =
      XCreateSimpleWindow(display, DefaultRootWindow(display), -10, -10, 1, 1,
                          0, 0, 0);

  expect(service->set_files({file.string()}), "set_files accepts a real path");
  const std::string expected = "file://" + file.string();
  // The backend takes ownership on its own thread. A clipboard manager may
  // already own the selection and keep answering with the previous content,
  // so poll until the *new* payload is served rather than looking at the
  // owner id alone.
  std::string uri;
  for (int attempt = 0; attempt < 15; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    uri = read_target(display, window, clipboard, uri_list, property, 300);
    if (uri.find(expected) != std::string::npos) break;
  }
  expect(uri.find(expected) != std::string::npos,
         "text/uri-list names the staged file");
  expect(uri.find("\r\n") != std::string::npos,
         "text/uri-list uses CRLF line endings");

  const std::string gnome_data =
      read_target(display, window, clipboard, gnome, property);
  expect(gnome_data.rfind("copy\n", 0) == 0,
         "gnome-copied-files starts with copy");
  expect(gnome_data.find("file://") != std::string::npos,
         "gnome-copied-files carries the file URI");

  const std::string targets = read_target(
      display, window, clipboard, XInternAtom(display, "TARGETS", False),
      property);
  (void)targets;  // TARGETS is 32-bit atoms; the content checks above matter.

  XDestroyWindow(display, window);
  XCloseDisplay(display);
  std::filesystem::remove_all(dir, ec);
  std::printf("%d checks, %d failure(s)\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
