// moLink desktop shell — GTK 3 + WebKitGTK 4.1.
//
// Starts molink-core, reads its ready line, then loads the UI that the core
// serves at "/" into a system WebView. No Rust, no bundled browser engine.

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#ifdef MOLINK_HAVE_XI2
#include <X11/extensions/XInput2.h>
#endif
#endif
#include <glib-unix.h>
#include <webkit2/webkit2.h>
#include <cairo.h>
#include <jsc/jsc.h>

#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdarg>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

pid_t g_core_pid = -1;
bool g_smoke = false;
bool g_smoke_done = false;
int g_exit_code = 0;
std::string g_core_host;
bool g_core_tls = false;

// Single-instance guard: the first shell listens on a per-user Unix socket;
// later launches send "show" and exit. Pop-outs (--url) and smoke tests are
// deliberately exempt.
int g_instance_fd = -1;
std::string g_instance_path;
GtkWidget* g_main_window = nullptr;
WebKitWebView* g_web_view = nullptr;

// --- drag-out --------------------------------------------------------------
//
// The page cannot start an OS-level drag from remote bytes. It downloads the
// selection into a per-gesture cache folder and posts `drag-out:<json>`; this
// side starts a real text/uri-list drag at the pointer. If the user cancels,
// the cache stays for five minutes (the retry may still be dragging); after a
// successful drop it is removed one minute later, when the target has read it.
struct DragOutState {
  std::string dir;
  std::vector<std::string> uris;
  std::string plain;
  std::string copied;
  guint cleanup_source = 0;
};

DragOutState* g_drag_out = nullptr;

// --- X11 keyboard grab -----------------------------------------------------
//
// The desktop environment's window manager owns Super/Alt combos, so the page
// never sees them. While the remote picture is focused the page may ask us to
// take an active X11 keyboard grab: every key then reaches our window (and
// WebKit) instead of the WM, and the existing event.code forwarding sends
// Win+R and friends to the remote unchanged. Wayland has no equivalent hook
// for an application, so this path is X11-only.
//
// The grab is a privileged state: a missed release locks the user out of their
// own keyboard. Every exit path below is deliberate.
constexpr gint64 kGrabDebounceUsec = 300 * 1000;

bool g_keyboard_grabbed = false;
/// XI2 device id of the active keyboard grab, -1 when no grab is held.
int g_xi_device_id = -1;
/// The page still wants the grab, even if a focus blip made us drop it.
bool g_grab_requested = false;
/// Swallow the Escape key release after the local escape combo fired.
bool g_swallow_escape_release = false;
gint64 g_grab_started_us = 0;
guint g_grab_release_source = 0;
guint g_grab_watchdog_source = 0;
bool g_grab_debounce_noted = false;

void shell_log_grab(const char* format, ...) {
  std::FILE* log = std::fopen("/tmp/molink-shell.log", "a");
  if (log == nullptr) return;
  std::fprintf(log, "[grab %.3f] ", g_get_real_time() / 1000000.0);
  va_list args;
  va_start(args, format);
  std::vfprintf(log, format, args);
  va_end(args);
  std::fputc('\n', log);
  std::fclose(log);
}

void shell_eval_js(const char* script) {
  if (g_web_view == nullptr) return;
  webkit_web_view_evaluate_javascript(g_web_view, script, -1, nullptr, nullptr,
                                      nullptr, nullptr, nullptr);
}

bool session_is_wayland() {
  const char* type = g_getenv("XDG_SESSION_TYPE");
  if (type != nullptr && g_ascii_strcasecmp(type, "wayland") == 0) return true;
  const char* display = g_getenv("WAYLAND_DISPLAY");
  return display != nullptr && display[0] != '\0';
}

/// X11 is the only backend where an application grab can intercept the
/// desktop's own shortcuts. A Wayland session is refused even if GTK happens
/// to run through XWayland, because the compositor keeps Super/Alt+Tab.
bool keyboard_grab_supported(GtkWidget* window) {
#if defined(GDK_WINDOWING_X11) && defined(MOLINK_HAVE_XI2)
  if (session_is_wayland()) return false;
  GdkDisplay* display = window != nullptr ? gtk_widget_get_display(window)
                                           : gdk_display_get_default();
  return display != nullptr && GDK_IS_X11_DISPLAY(display);
#else
  (void)window;
  return false;
#endif
}

void report_grab_status(const char* status) {
  gchar* script = g_strdup_printf(
      "window.dispatchEvent(new CustomEvent('molink-grab',{detail:'%s'}))",
      status);
  shell_eval_js(script);
  g_free(script);
}

/// Tells the page (which may already have mounted) whether this shell can grab
/// at all, and leaves the answer readable synchronously.
void publish_grab_support() {
  const bool supported = keyboard_grab_supported(g_main_window);
  const char* status = supported ? "off" : "unsupported";
  gchar* script = g_strdup_printf(
      "window.__molinkGrabSupported=%s;"
      "window.dispatchEvent(new CustomEvent('molink-grab',{detail:'%s'}));",
      supported ? "true" : "false", status);
  shell_eval_js(script);
  g_free(script);
  shell_log_grab("support=%s", supported ? "x11" : "unsupported");
}

void release_keyboard_grab(const char* reason) {
  if (g_grab_release_source != 0) {
    g_source_remove(g_grab_release_source);
    g_grab_release_source = 0;
  }
  if (g_grab_watchdog_source != 0) {
    g_source_remove(g_grab_watchdog_source);
    g_grab_watchdog_source = 0;
  }
  if (!g_keyboard_grabbed) return;
#if defined(GDK_WINDOWING_X11) && defined(MOLINK_HAVE_XI2)
  GdkDisplay* display = g_main_window != nullptr
                            ? gtk_widget_get_display(g_main_window)
                            : gdk_display_get_default();
  if (display != nullptr && GDK_IS_X11_DISPLAY(display) &&
      g_xi_device_id >= 0) {
    XIUngrabDevice(GDK_DISPLAY_XDISPLAY(display), g_xi_device_id,
                   CurrentTime);
  }
  g_xi_device_id = -1;
#endif
  const gint64 held_ms =
      (g_get_monotonic_time() - g_grab_started_us) / 1000;
  g_keyboard_grabbed = false;
  g_grab_started_us = 0;
  shell_log_grab("release (%s) after %" G_GINT64_FORMAT " ms", reason,
                 held_ms);
  // The grab may have swallowed part of a local shortcut (notably the escape
  // combo). Let the page release every key it already forwarded, so the
  // remote never keeps Ctrl/Alt/Shift held.
  shell_eval_js(
      "if(window.__molinkReleaseKeys)window.__molinkReleaseKeys()");
  report_grab_status("off");
}

gboolean grab_watchdog(gpointer data);

void grab_keyboard(GtkWidget* window) {
  if (window == nullptr) return;
  if (g_keyboard_grabbed) {
    shell_log_grab("grab ignored: already grabbed");
    report_grab_status("on");
    return;
  }
  g_grab_requested = true;
  if (!keyboard_grab_supported(window)) {
    shell_log_grab("grab refused: %s",
                   session_is_wayland() ? "wayland" : "not x11");
    report_grab_status("unsupported");
    return;
  }
#if defined(GDK_WINDOWING_X11) && defined(MOLINK_HAVE_XI2)
  // An inactive window must not grab: nothing would later deliver the
  // focus-out that releases it.
  if (!gtk_window_is_active(GTK_WINDOW(window))) {
    shell_log_grab("grab deferred: window is not active");
    report_grab_status("off");
    return;
  }
  if (!gtk_widget_get_realized(window)) gtk_widget_realize(window);
  GdkWindow* gdk_window = gtk_widget_get_window(window);
  if (gdk_window == nullptr) {
    shell_log_grab("grab failed: window has no GdkWindow");
    report_grab_status("failed");
    return;
  }
  GdkDisplay* display = gtk_widget_get_display(window);
  Display* xdisplay = GDK_DISPLAY_XDISPLAY(display);
  const Window xid = gdk_x11_window_get_xid(gdk_window);
  // XGrabKeyboard is a core-protocol grab. GTK3 consumes key events through
  // XInput2, and a core grab's events (delivered to the grab window rather
  // than the focus window) never reach gtk_widget key-press-event: the X
  // events arrive, but GDK drops them, so WebKit sees nothing and even the
  // local escape combo cannot fire. An XI2 grab on the master keyboard is
  // translated by GDK normally and still overrides the WM's Super/Alt grabs.
  int device = -1;
  int device_count = 0;
  XIDeviceInfo* devices =
      XIQueryDevice(xdisplay, XIAllMasterDevices, &device_count);
  if (devices != nullptr) {
    for (int i = 0; i < device_count; ++i) {
      if (devices[i].use == XIMasterKeyboard) {
        device = devices[i].deviceid;
        break;
      }
    }
    XIFreeDeviceInfo(devices);
  }
  if (device < 0) {
    shell_log_grab("grab failed: no master keyboard");
    report_grab_status("failed");
    return;
  }
  unsigned char key_mask_bits[XIMaskLen(XI_LASTEVENT)] = {0};
  XISetMask(key_mask_bits, XI_KeyPress);
  XISetMask(key_mask_bits, XI_KeyRelease);
  XIEventMask event_mask;
  event_mask.deviceid = XIAllMasterDevices;
  event_mask.mask_len = XIMaskLen(XI_LASTEVENT);
  event_mask.mask = key_mask_bits;
  const int status =
      XIGrabDevice(xdisplay, device, xid, CurrentTime, None, GrabModeAsync,
                   GrabModeAsync, False, &event_mask);
  if (status != GrabSuccess) {
    const char* name = "unknown";
    switch (status) {
      case AlreadyGrabbed: name = "AlreadyGrabbed"; break;
      case GrabInvalidTime: name = "GrabInvalidTime"; break;
      case GrabNotViewable: name = "GrabNotViewable"; break;
      case GrabFrozen: name = "GrabFrozen"; break;
      default: break;
    }
    shell_log_grab("grab failed: %s (%d)", name, status);
    report_grab_status("failed");
    return;
  }
  g_xi_device_id = device;
  g_keyboard_grabbed = true;
  g_grab_debounce_noted = false;
  g_grab_started_us = g_get_monotonic_time();
  // FocusOut is not dependable while the XI2 grab is active; poll the X focus
  // so a window switch can never leave the desktop keyboard captured.
  g_grab_watchdog_source = g_timeout_add(200, grab_watchdog, window);
  shell_log_grab("grab ok xid=0x%lx device=%d",
                 static_cast<unsigned long>(xid), device);
  report_grab_status("on");
#else
  (void)window;
#endif
}

/// True while the X input focus still belongs to this toplevel (GTK's focus
/// proxy is a child, so walk up). An active XI2 grab can hide FocusOut from
/// GTK, which makes a polled check the only reliable release trigger.
bool x_focus_is_ours(GtkWidget* window) {
#if defined(GDK_WINDOWING_X11) && defined(MOLINK_HAVE_XI2)
  GdkWindow* gdk_window = gtk_widget_get_window(window);
  if (gdk_window == nullptr) return false;
  Display* display = GDK_DISPLAY_XDISPLAY(gtk_widget_get_display(window));
  const Window ours = gdk_x11_window_get_xid(gdk_window);
  Window focus = None;
  int revert = RevertToNone;
  XGetInputFocus(display, &focus, &revert);
  while (focus != None && focus != PointerRoot) {
    if (focus == ours) return true;
    if (focus == DefaultRootWindow(display)) return false;
    Window root = None;
    Window parent = None;
    Window* children = nullptr;
    unsigned int count = 0;
    if (!XQueryTree(display, focus, &root, &parent, &children, &count)) {
      return false;
    }
    if (children != nullptr) XFree(children);
    if (parent == focus || parent == None) return false;
    focus = parent;
  }
  return false;
#else
  (void)window;
  return false;
#endif
}

gboolean grab_watchdog(gpointer data) {
  if (!g_keyboard_grabbed) {
    g_grab_watchdog_source = 0;
    return G_SOURCE_REMOVE;
  }
  GtkWidget* window = GTK_WIDGET(data);
  // Same debounce as the focus handler: X11 can flicker focus for a moment
  // right after a grab, and releasing there would undo the grab instantly.
  if (g_get_monotonic_time() - g_grab_started_us < kGrabDebounceUsec) {
    if (!g_grab_debounce_noted && !x_focus_is_ours(window)) {
      g_grab_debounce_noted = true;
      shell_log_grab("focus left during 300 ms debounce; holding grab");
    }
    return G_SOURCE_CONTINUE;
  }
  GdkWindow* gdk_window = gtk_widget_get_window(window);
  const bool iconified =
      gdk_window != nullptr &&
      (gdk_window_get_state(gdk_window) & GDK_WINDOW_STATE_ICONIFIED) != 0;
  if (iconified || !gtk_window_is_active(GTK_WINDOW(window)) ||
      !x_focus_is_ours(window)) {
    g_grab_watchdog_source = 0;  // release() must not remove this source
    release_keyboard_grab(iconified ? "watchdog: iconified"
                                    : "watchdog: focus left the window");
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

gboolean grab_release_check(gpointer data) {
  g_grab_release_source = 0;
  if (!g_keyboard_grabbed) return G_SOURCE_REMOVE;
  GtkWidget* window = GTK_WIDGET(data);
  GdkWindow* gdk_window = gtk_widget_get_window(window);
  const bool iconified =
      gdk_window != nullptr &&
      (gdk_window_get_state(gdk_window) & GDK_WINDOW_STATE_ICONIFIED) != 0;
  if (!gtk_window_is_active(GTK_WINDOW(window)) || !x_focus_is_ours(window) ||
      iconified) {
    release_keyboard_grab(iconified ? "iconified (debounced)"
                                    : "focus-out (debounced)");
  } else {
    // The grab itself makes X11 focus flicker right after it starts; the
    // window is still active, so keep it.
    shell_log_grab("focus jitter settled; keeping grab");
  }
  return G_SOURCE_REMOVE;
}

gboolean on_grab_focus_out(GtkWidget* window, GdkEventFocus*, gpointer) {
  if (!g_keyboard_grabbed) return FALSE;
  const gint64 elapsed_us = g_get_monotonic_time() - g_grab_started_us;
  if (elapsed_us < kGrabDebounceUsec) {
    // RustDesk sees ~10 Hz focus-out/focus-in pairs on X11 right after a
    // grab. Do not release on the first one: re-check once the debounce
    // window has passed, and release only if the window is still unfocused.
    const gint64 remaining_ms = (kGrabDebounceUsec - elapsed_us + 999) / 1000;
    if (g_grab_release_source == 0) {
      shell_log_grab("focus-out %" G_GINT64_FORMAT
                     " ms after grab; re-check in %" G_GINT64_FORMAT " ms",
                     elapsed_us / 1000, remaining_ms);
      g_grab_release_source = g_timeout_add(
          static_cast<guint>(remaining_ms), grab_release_check, window);
    } else {
      shell_log_grab("focus-out within debounce; already scheduled");
    }
    return FALSE;
  }
  release_keyboard_grab("focus-out");
  return FALSE;
}

gboolean grab_again_idle(gpointer data) {
  GtkWidget* window = GTK_WIDGET(data);
  if (g_grab_requested && !g_keyboard_grabbed &&
      gtk_window_is_active(GTK_WINDOW(window))) {
    shell_log_grab("window active again; restoring requested grab");
    grab_keyboard(window);
  }
  return G_SOURCE_REMOVE;
}

gboolean on_grab_focus_in(GtkWidget* window, GdkEventFocus*, gpointer) {
  if (g_grab_release_source != 0) {
    g_source_remove(g_grab_release_source);
    g_grab_release_source = 0;
    shell_log_grab("focus returned within debounce; release cancelled");
  }
  // An auto-release (focus-out/unmap/iconify) keeps the page's request alive;
  // once the window is active again the remote picture is still focused, so
  // restore the grab. Deferred out of this signal to avoid re-entrancy.
  if (!g_keyboard_grabbed && g_grab_requested) {
    g_idle_add(grab_again_idle, window);
  }
  return FALSE;
}

void on_grab_unmap(GtkWidget*, gpointer) {
  release_keyboard_grab("unmap");
}

/// Runs before GTK delivers the key to the focused WebView, so the local
/// escape combo cannot be swallowed by the page or the remote.
gint on_grab_key_snooper(GtkWidget*, GdkEventKey* event, gpointer) {
  if (event == nullptr) return FALSE;
  if (event->type == GDK_KEY_RELEASE && g_swallow_escape_release &&
      event->keyval == GDK_KEY_Escape) {
    g_swallow_escape_release = false;
    return TRUE;
  }
  if (event->type != GDK_KEY_PRESS || !g_keyboard_grabbed) return FALSE;
  const guint required = GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SHIFT_MASK;
  if ((event->state & required) != required) return FALSE;
  if (event->keyval != GDK_KEY_Escape) return FALSE;
  shell_log_grab("escape combo pressed; releasing local grab");
  g_swallow_escape_release = true;
  g_grab_requested = false;
  release_keyboard_grab("escape combo");
  return TRUE;  // The page must not forward half of a shortcut.
}

std::string exe_dir() {
  std::error_code ec;
  const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
  if (ec) return std::string();
  return exe.parent_path().string();
}

std::string first_existing(const std::vector<fs::path>& candidates) {
  std::error_code ec;
  for (const fs::path& candidate : candidates) {
    if (fs::is_regular_file(candidate, ec)) return candidate.lexically_normal().string();
  }
  return std::string();
}

std::string find_core() {
  if (const char* env = std::getenv("MOLINK_CORE")) {
    if (fs::is_regular_file(env)) return env;
  }
  const std::string dir = exe_dir();
  std::vector<fs::path> candidates;
  if (!dir.empty()) {
    candidates.push_back(fs::path(dir) / "molink-core");
    candidates.push_back(fs::path(dir) / "../lib/molink/molink-core");
    candidates.push_back(fs::path(dir) / "../../../backend/build/molink-core");
    candidates.push_back(fs::path(dir) / "../../../backend/build/Release/molink-core");
  }
  candidates.push_back("backend/build/molink-core");
  candidates.push_back("../backend/build/molink-core");
  candidates.push_back("../../backend/build/molink-core");
  return first_existing(candidates);
}

std::string find_web_root() {
  if (const char* env = std::getenv("MOLINK_WEB_ROOT")) {
    if (fs::is_regular_file(fs::path(env) / "index.html")) return env;
  }
  const std::string dir = exe_dir();
  std::vector<fs::path> candidates;
  if (!dir.empty()) {
    candidates.push_back(fs::path(dir) / "web");
    candidates.push_back(fs::path(dir) / "../web");
    candidates.push_back(fs::path(dir) / "../share/molink/web");
    candidates.push_back(fs::path(dir) / "../../../frontend/dist");
  }
  candidates.push_back("frontend/dist");
  candidates.push_back("../frontend/dist");
  candidates.push_back("../../frontend/dist");
  for (const fs::path& candidate : candidates) {
    std::error_code ec;
    if (fs::is_regular_file(candidate / "index.html", ec)) {
      return candidate.lexically_normal().string();
    }
  }
  return std::string();
}

bool json_number(const std::string& json, const char* key, long& out) {
  const std::string needle = std::string("\"") + key + "\":";
  std::size_t pos = json.find(needle);
  if (pos == std::string::npos) return false;
  pos += needle.size();
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
  long value = 0;
  std::size_t digits = 0;
  while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
    value = value * 10 + (json[pos] - '0');
    ++pos;
    ++digits;
  }
  if (digits == 0) return false;
  out = value;
  return true;
}

bool json_string(const std::string& json, const char* key, std::string& out) {
  const std::string needle = std::string("\"") + key + "\":\"";
  std::size_t pos = json.find(needle);
  if (pos == std::string::npos) return false;
  pos += needle.size();
  const std::size_t end = json.find('"', pos);
  if (end == std::string::npos) return false;
  out = json.substr(pos, end - pos);
  return true;
}

std::string url_host(const std::string& host) {
  if (host.empty()) return "127.0.0.1";
  if (host.front() == '[' && host.back() == ']') return host;
  return host.find(':') != std::string::npos ? "[" + host + "]" : host;
}

std::string ui_host() {
  if (g_core_host.empty() || g_core_host == "0.0.0.0") return "127.0.0.1";
  if (g_core_host == "::") return "::1";
  return g_core_host;
}

bool start_core(const std::string& core, const std::string& web_root, int& port,
                std::string& token, std::string& client_token, bool& tls,
                std::string& surface_path, std::string& err) {
  // The core owns the port: it uses the persisted setting or 28418.
  std::vector<std::string> args = {core, "--print-ready", "--parent-pid",
                                   std::to_string(getpid())};
  if (!web_root.empty()) {
    args.push_back("--web-root");
    args.push_back(web_root);
  }
  if (!g_core_host.empty()) {
    args.push_back("--host");
    args.push_back(g_core_host);
  }

  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (std::string& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
  argv.push_back(nullptr);

  int stdout_fd = -1;
  GError* error = nullptr;
  const gboolean spawned = g_spawn_async_with_pipes(
      nullptr, argv.data(), nullptr,
      static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH |
                               G_SPAWN_DO_NOT_REAP_CHILD),
      nullptr, nullptr,
      &g_core_pid, nullptr, &stdout_fd, nullptr, &error);
  if (!spawned) {
    err = std::string("cannot start molink-core: ") +
          (error != nullptr ? error->message : "unknown error");
    if (error != nullptr) g_error_free(error);
    g_core_pid = -1;
    return false;
  }

  std::string line;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  bool complete = false;
  // libuvcpp's console logger writes startup lines to stdout ahead of the
  // ready JSON. Stopping at the first newline parsed a log line and left the
  // shell without a core (no port, no client token, no surface).
  while (!complete && std::chrono::steady_clock::now() < deadline) {
    const size_t marker = line.find("\"event\":\"ready\"");
    if (marker != std::string::npos &&
        line.find('\n', marker) != std::string::npos) {
      complete = true;
      break;
    }
    struct pollfd descriptor;
    descriptor.fd = stdout_fd;
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    const int ready = ::poll(&descriptor, 1, 200);
    if (ready <= 0) continue;
    char buffer[4096];
    const ssize_t got = ::read(stdout_fd, buffer, sizeof(buffer));
    if (got <= 0) break;
    line.append(buffer, static_cast<std::size_t>(got));
    if (line.size() > 256 * 1024) break;
  }
  if (stdout_fd >= 0) ::close(stdout_fd);
  // Keep only the ready JSON. nlohmann sorts keys, so the line starts with
  // "capture", not "event"; take the object containing the marker.
  {
    const size_t marker = line.find("\"event\":\"ready\"");
    if (marker != std::string::npos) {
      const size_t previous_newline = line.rfind('\n', marker);
      const size_t start = previous_newline == std::string::npos
                               ? 0
                               : previous_newline + 1;
      const size_t brace = line.find('{', start);
      const size_t end = line.find('\n', marker);
      if (brace != std::string::npos) {
        line = line.substr(brace, end == std::string::npos
                                      ? std::string::npos
                                      : end - brace);
      }
    }
  }

  long parsed_port = 0;
  if (!complete || line.empty() || line.front() != '{' ||
      !json_number(line, "port", parsed_port) ||
      !json_string(line, "token", token) ||
      !json_string(line, "client_token", client_token) || parsed_port <= 0) {
    err = "molink-core did not report a ready port and tokens";
    return false;
  }
  port = static_cast<int>(parsed_port);
  tls = line.find("\"tls\":true") != std::string::npos;
  // Set only when MOLINK_SURFACE asked the core to publish a native picture.
  json_string(line, "surface", surface_path);
  return true;
}

void stop_core() {
  if (g_core_pid <= 0) return;
  ::kill(g_core_pid, SIGTERM);
  for (int i = 0; i < 30; ++i) {
    int status = 0;
    const pid_t result = ::waitpid(g_core_pid, &status, WNOHANG);
    if (result == g_core_pid) {
      g_core_pid = -1;
      return;
    }
    ::usleep(100 * 1000);
  }
  ::kill(g_core_pid, SIGKILL);
  int status = 0;
  ::waitpid(g_core_pid, &status, 0);
  g_core_pid = -1;
}

gboolean check_core(gpointer) {
  if (g_core_pid <= 0) return G_SOURCE_CONTINUE;
  int status = 0;
  const pid_t result = ::waitpid(g_core_pid, &status, WNOHANG);
  if (result == g_core_pid) {
    g_core_pid = -1;
    std::fprintf(stderr, "moLink shell: molink-core exited (status %d)\n",
                 status);
    if (!g_smoke_done) {
      g_exit_code = 1;
      gtk_main_quit();
    }
  }
  return G_SOURCE_CONTINUE;
}

void on_destroy(GtkWidget*, gpointer) {
  // The view may be on its way out with the window; do not evaluate into it.
  g_web_view = nullptr;
  release_keyboard_grab("window destroy");
  gtk_main_quit();
}

gboolean on_quit_signal(gpointer) {
  release_keyboard_grab("quit signal");
  gtk_main_quit();
  return G_SOURCE_CONTINUE;
}

/// GtkWindow normally allocates its only child the whole content area, but
/// after a WM-driven maximize/fullscreen the WebKit view can keep its old
/// allocation (the rest of the window stays transparent). Re-allocating it
/// from the window's own allocation closes that gap.
void force_child_allocate(GtkWidget* window) {
  GtkWidget* child = gtk_bin_get_child(GTK_BIN(window));
  if (child == nullptr) return;
  // gtk_widget_get_allocation() can still report the pre-maximize size at
  // this point; the GdkWindow already has the size the WM gave us.
  int width = 0;
  int height = 0;
  GdkWindow* gdk_window = gtk_widget_get_window(window);
  if (gdk_window != nullptr) {
    width = gdk_window_get_width(gdk_window);
    height = gdk_window_get_height(gdk_window);
  }
  if (width <= 0 || height <= 0) {
    GtkAllocation allocation;
    gtk_widget_get_allocation(window, &allocation);
    width = allocation.width;
    height = allocation.height;
  }
  if (width <= 0 || height <= 0) return;
  GtkAllocation current;
  gtk_widget_get_allocation(child, &current);
  if (current.width == width && current.height == height) return;
  if (std::FILE* log = std::fopen("/tmp/molink-shell.log", "a")) {
    std::fprintf(log, "reallocate webview %dx%d -> %dx%d\n", current.width,
                 current.height, width, height);
    std::fclose(log);
  }
  GtkAllocation child_allocation = {0, 0, width, height};
  gtk_widget_size_allocate(child, &child_allocation);
}

void log_viewport_result(GObject* source, GAsyncResult* result, gpointer) {
  GError* error = nullptr;
  JSCValue* value = webkit_web_view_evaluate_javascript_finish(
      WEBKIT_WEB_VIEW(source), result, &error);
  if (error != nullptr) {
    g_error_free(error);
    return;
  }
  if (value != nullptr) {
    char* text = jsc_value_to_string(value);
    if (text != nullptr) {
      if (std::FILE* log = std::fopen("/tmp/molink-shell.log", "a")) {
        std::fprintf(log, "viewport %s\n", text);
        std::fclose(log);
      }
      g_free(text);
    }
    g_object_unref(value);
  }
}

gboolean log_viewport(gpointer data) {
  WebKitWebView* view = WEBKIT_WEB_VIEW(data);
  GtkWidget* toplevel = gtk_widget_get_toplevel(GTK_WIDGET(view));
  if (GTK_IS_WINDOW(toplevel)) {
    GdkWindow* gdk_window = gtk_widget_get_window(toplevel);
    GtkWidget* child = gtk_bin_get_child(GTK_BIN(toplevel));
    GtkAllocation allocation = {0, 0, 0, 0};
    if (child != nullptr) gtk_widget_get_allocation(child, &allocation);
    if (gdk_window != nullptr) {
      if (std::FILE* log = std::fopen("/tmp/molink-shell.log", "a")) {
        std::fprintf(log, "window %dx%d child %dx%d\n",
                     gdk_window_get_width(gdk_window),
                     gdk_window_get_height(gdk_window), allocation.width,
                     allocation.height);
        std::fclose(log);
      }
    }
  }
  webkit_web_view_evaluate_javascript(
      view,
      "window.innerWidth + 'x' + window.innerHeight + ' dpr=' + "
      "window.devicePixelRatio",
      -1, nullptr, nullptr, nullptr, log_viewport_result, nullptr);
  return G_SOURCE_CONTINUE;
}

void on_load_changed(WebKitWebView* view, WebKitLoadEvent event, gpointer) {
  if (event != WEBKIT_LOAD_FINISHED) return;
  g_web_view = view;
  // A reload or navigation replaces the page object: tell the new one whether
  // this shell can grab before it draws the settings switch.
  publish_grab_support();
  // A window that the WM maximized before the page loaded can miss the
  // resize; reallocate once the page is up as well.
  GtkWidget* toplevel = gtk_widget_get_toplevel(GTK_WIDGET(view));
  if (GTK_IS_WINDOW(toplevel)) {
    g_timeout_add(100,
                  [](gpointer data) -> gboolean {
                    GtkWidget* window = GTK_WIDGET(data);
                    GtkWidget* child = gtk_bin_get_child(GTK_BIN(window));
                    GdkWindow* gdk_window = gtk_widget_get_window(window);
                    if (child == nullptr || gdk_window == nullptr) {
                      return G_SOURCE_REMOVE;
                    }
                    const int width = gdk_window_get_width(gdk_window);
                    const int height = gdk_window_get_height(gdk_window);
                    if (width <= 0 || height <= 1) return G_SOURCE_REMOVE;
                    // The child allocation may already be correct while the
                    // page viewport is still the pre-maximize size (the
                    // WebView was created before the WM resized the window).
                    // Allocate one pixel less and then the real size so
                    // WebKit always sees a resize after the page is loaded.
                    GtkAllocation bump = {0, 0, width, height - 1};
                    gtk_widget_size_allocate(child, &bump);
                    GtkAllocation final_allocation = {0, 0, width, height};
                    gtk_widget_size_allocate(child, &final_allocation);
                    return G_SOURCE_REMOVE;
                  },
                  toplevel);
  }
  if (!g_smoke) return;
  const char* title = webkit_web_view_get_title(view);
  std::printf("SHELL_SMOKE_OK title=%s\n", title != nullptr ? title : "");
  std::fflush(stdout);
  g_smoke_done = true;
  gtk_main_quit();
}

gboolean on_load_failed(WebKitWebView*, WebKitLoadEvent, const gchar* uri,
                        GError* error, gpointer) {
  std::fprintf(stderr, "moLink shell: failed to load %s: %s\n", uri,
               error != nullptr ? error->message : "unknown error");
  if (g_smoke) {
    std::printf("SHELL_SMOKE_FAIL %s\n", error != nullptr ? error->message : "");
    std::fflush(stdout);
    g_smoke_done = true;
    g_exit_code = 1;
    gtk_main_quit();
  }
  return FALSE;
}

gboolean on_permission_request(WebKitWebView* view,
                               WebKitPermissionRequest* request,
                               gpointer origin) {
  const char* uri = webkit_web_view_get_uri(view);
  const char* allowed = static_cast<const char*>(origin);
  if (uri != nullptr && allowed != nullptr && g_str_has_prefix(uri, allowed)) {
    webkit_permission_request_allow(request);
  } else {
    webkit_permission_request_deny(request);
  }
  return TRUE;
}

gboolean on_tls_error(WebKitWebView* view, const gchar* failing_uri,
                      GTlsCertificate* certificate, GTlsCertificateFlags,
                      gpointer) {
  if (view == nullptr || failing_uri == nullptr || certificate == nullptr) {
    return FALSE;
  }
  // The shell started this core and the user opted into LAN encryption; trust
  // its self-signed certificate for the host it was loaded from. A browser
  // without the shell still shows the normal certificate warning.
  std::string host = ui_host();
  GUri* uri = g_uri_parse(failing_uri, G_URI_FLAGS_NONE, nullptr);
  if (uri != nullptr) {
    const gchar* parsed = g_uri_get_host(uri);
    if (parsed != nullptr && *parsed != '\0') host = parsed;
    g_uri_unref(uri);
  }
  if (WebKitWebContext* context = webkit_web_view_get_context(view)) {
    webkit_web_context_allow_tls_certificate_for_host(context, certificate,
                                                      host.c_str());
    webkit_web_view_load_uri(view, failing_uri);
    return TRUE;
  }
  return FALSE;
}

gboolean on_key_press(GtkWidget* window, GdkEventKey* event, gpointer) {
  if (event->keyval == GDK_KEY_F11) {
    static bool fullscreen = false;
    fullscreen = !fullscreen;
    if (fullscreen) {
      gtk_window_fullscreen(GTK_WINDOW(window));
    } else {
      gtk_window_unfullscreen(GTK_WINDOW(window));
    }
    return TRUE;
  }
  if ((event->state & GDK_CONTROL_MASK) && (event->state & GDK_SHIFT_MASK) &&
      (event->keyval == GDK_KEY_q || event->keyval == GDK_KEY_Q)) {
    gtk_widget_destroy(window);
    return TRUE;
  }
  return FALSE;
}

gboolean smoke_timeout(gpointer) {
  if (g_smoke && !g_smoke_done) {
    std::printf("SHELL_SMOKE_FAIL timeout\n");
    std::fflush(stdout);
    g_smoke_done = true;
    g_exit_code = 1;
    gtk_main_quit();
  }
  return G_SOURCE_REMOVE;
}

}  // namespace


GtkStatusIcon* g_tray_icon = nullptr;
GtkWidget* g_tray_menu = nullptr;
GtkWidget* g_tray_show_item = nullptr;
GtkWidget* g_tray_quit_item = nullptr;
/// Native picture window over the core's shared-memory frame surface
/// (surface_window.cpp). Defined there to keep this file about the web shell.
int run_surface_window(const std::string& path);

bool g_url_mode = false;
std::string g_url_override;
std::string g_surface_path;

std::string exe_path() {
  char buffer[4096];
  const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (length <= 0) return std::string();
  buffer[length] = '\0';
  return std::string(buffer);
}

GtkWidget* on_create_web_view(WebKitWebView*, WebKitNavigationAction* action,
                              gpointer) {
  WebKitURIRequest* request = webkit_navigation_action_get_request(action);
  if (request == nullptr) return nullptr;
  const char* uri = webkit_uri_request_get_uri(request);
  if (uri == nullptr || !g_str_has_prefix(uri, "http://127.0.0.1:")) {
    return nullptr;
  }
  const std::string executable = exe_path();
  if (executable.empty()) return nullptr;
  char* args[] = {const_cast<char*>(executable.c_str()),
                  const_cast<char*>("--url"),
                  const_cast<char*>(uri), nullptr};
  g_spawn_async(nullptr, args, nullptr,
                static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH), nullptr, nullptr,
                nullptr, nullptr);
  return nullptr;
}

std::string find_icon() {
  if (const char* env = std::getenv("MOLINK_ICON")) {
    if (fs::is_regular_file(env)) return env;
  }
  const std::string dir = exe_dir();
  std::vector<fs::path> candidates;
  if (!dir.empty()) {
    candidates.push_back(fs::path(dir) / "molink.png");
    candidates.push_back(fs::path(dir) / "../share/icons/hicolor/256x256/apps/molink.png");
    candidates.push_back(fs::path(dir) / "../share/molink/molink.png");
    candidates.push_back("/usr/share/icons/hicolor/256x256/apps/molink.png");
    candidates.push_back(fs::path(dir) / "../../../shell/assets/icons/icon.png");
  }
  candidates.push_back("shell/assets/icons/icon.png");
  for (const fs::path& candidate : candidates) {
    std::error_code ec;
    if (fs::is_regular_file(candidate, ec)) return candidate.string();
  }
  return std::string();
}

void rounded_rect_path(cairo_t* cr, double x, double y, double w, double h,
                       double r) {
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2.0, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2.0);
  cairo_arc(cr, x + r, y + h - r, r, G_PI / 2.0, G_PI);
  cairo_arc(cr, x + r, y + r, r, G_PI, 3.0 * G_PI / 2.0);
  cairo_close_path(cr);
}

bool g_window_square = false;

/// Replace the rounded region with one covering the whole window. Resetting
/// with NULL does not reliably drop an already installed region in GTK3, so
/// a maximized window could stay clipped to its old size.
void apply_square_shape(GtkWidget* widget) {
  int width = 0;
  int height = 0;
  GdkWindow* gdk_window = gtk_widget_get_window(widget);
  if (gdk_window != nullptr) {
    width = gdk_window_get_width(gdk_window);
    height = gdk_window_get_height(gdk_window);
  }
  if (width <= 0 || height <= 0) {
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    width = allocation.width;
    height = allocation.height;
  }
  if (width <= 0 || height <= 0) {
    gtk_widget_shape_combine_region(widget, nullptr);
    return;
  }
  cairo_rectangle_int_t rect = {0, 0, width, height};
  cairo_region_t* region = cairo_region_create_rectangle(&rect);
  gtk_widget_shape_combine_region(widget, region);
  cairo_region_destroy(region);
}

void apply_window_shape(GtkWidget* widget) {
  if (!gtk_widget_get_realized(widget)) return;
  // Rounded outer corners used to be installed as a shape region. Any stale
  // region clips the whole window (the maximized window kept the region from
  // its default size), which is far worse than square corners. Always cover
  // the complete window instead.
  apply_square_shape(widget);
}

gboolean on_size_allocate(GtkWidget* widget, GdkRectangle*, gpointer) {
  GdkWindow* gdk_window = gtk_widget_get_window(widget);
  const GdkWindowState state =
      gdk_window != nullptr ? gdk_window_get_state(gdk_window)
                            : static_cast<GdkWindowState>(0);
  const bool square =
      g_window_square || (state & GDK_WINDOW_STATE_MAXIMIZED) != 0 ||
      (state & GDK_WINDOW_STATE_FULLSCREEN) != 0;
  if (square) {
    apply_square_shape(widget);
  } else {
    apply_window_shape(widget);
  }
  // Every allocation, including the one produced by a maximize at startup,
  // must reach the WebView.
  force_child_allocate(widget);
  return FALSE;
}

gboolean on_window_state(GtkWidget* widget, GdkEventWindowState* event,
                         gpointer) {
  // A minimized window must never keep the keyboard grabbed.
  if ((event->changed_mask & GDK_WINDOW_STATE_ICONIFIED) != 0 &&
      (event->new_window_state & GDK_WINDOW_STATE_ICONIFIED) != 0) {
    release_keyboard_grab("iconified");
  }
  const GdkWindowState square_mask = static_cast<GdkWindowState>(
      GDK_WINDOW_STATE_MAXIMIZED | GDK_WINDOW_STATE_FULLSCREEN);
  g_window_square = (event->new_window_state & square_mask) != 0;
  if (g_window_square) {
    apply_square_shape(widget);
  } else {
    apply_window_shape(widget);
  }
  // Maximize/fullscreen can race the WebView's allocation; ask GTK to lay the
  // window and its child out again so the remote canvas fills the new size.
  GtkWidget* child = gtk_bin_get_child(GTK_BIN(widget));
  if (child != nullptr) gtk_widget_queue_resize(child);
  gtk_widget_queue_resize(widget);
  gtk_widget_queue_draw(widget);
  // Let the WM finish the configure first, then make sure the WebView covers
  // the new client area even if GTK skipped the child reallocation.
  for (guint delay : {150u, 500u}) {
    g_timeout_add(delay,
                  [](gpointer data) -> gboolean {
                    force_child_allocate(GTK_WIDGET(data));
                    return G_SOURCE_REMOVE;
                  },
                  widget);
  }
  return FALSE;
}

void show_window(GtkWidget* window) {
  gtk_widget_show(window);
  gtk_window_deiconify(GTK_WINDOW(window));
  gtk_window_present(GTK_WINDOW(window));
}

std::string instance_socket_path() {
  std::string path;
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  if (runtime != nullptr && runtime[0] != '\0' &&
      ::access(runtime, W_OK) == 0) {
    path = std::string(runtime) + "/molink-shell.sock";
  }
  sockaddr_un limit{};
  if (path.empty() || path.size() >= sizeof(limit.sun_path)) {
    path = "/tmp/molink-shell-" + std::to_string(::getuid()) + ".sock";
  }
  return path;
}

int connect_instance(const std::string& path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
    return fd;
  }
  ::close(fd);
  return -1;
}

gboolean on_instance_accept(GIOChannel*, GIOCondition, gpointer) {
  const int fd = ::accept(g_instance_fd, nullptr, nullptr);
  if (fd >= 0) {
    char buffer[32];
    const ssize_t got = ::read(fd, buffer, sizeof(buffer));
    (void)got;
    ::close(fd);
  }
  if (g_main_window != nullptr) show_window(g_main_window);
  return G_SOURCE_CONTINUE;
}

/// Returns true when this process owns the instance socket. Sets
/// @p already_running when another shell accepted the "show" request.
bool claim_instance_socket(const std::string& path, bool& already_running) {
  already_running = false;
  for (int attempt = 0; attempt < 2; ++attempt) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
      if (::listen(fd, 4) != 0) {
        ::close(fd);
        return false;
      }
      ::chmod(path.c_str(), 0600);
      g_instance_fd = fd;
      g_instance_path = path;
      GIOChannel* channel = g_io_channel_unix_new(fd);
      g_io_add_watch(channel, G_IO_IN, on_instance_accept, nullptr);
      g_io_channel_unref(channel);
      return true;
    }

    const int error = errno;
    ::close(fd);
    if (error != EADDRINUSE) return false;

    const int client = connect_instance(path);
    if (client >= 0) {
      const ssize_t sent = ::write(client, "show\n", 5);
      (void)sent;
      ::close(client);
      already_running = true;
      return false;
    }
    // A killed shell can leave the socket file behind; it is not in use, so
    // remove it and retry the bind once.
    ::unlink(path.c_str());
  }
  return false;
}

void cleanup_instance_socket() {
  if (g_instance_fd >= 0) {
    ::close(g_instance_fd);
    g_instance_fd = -1;
  }
  if (!g_instance_path.empty()) ::unlink(g_instance_path.c_str());
}

void hide_to_tray(GtkWidget* window) {
  gtk_widget_hide(window);
  if (g_tray_icon != nullptr) gtk_status_icon_set_visible(g_tray_icon, TRUE);
}

/// The page mirrors its language into the core's ui.json; the native tray
/// cannot read the WebView's localStorage. Fall back to the environment
/// language when the file (or the key) is missing.
bool tray_uses_chinese() {
  gchar* path = g_build_filename(g_get_user_config_dir(), "molink", "ui.json",
                                 nullptr);
  gchar* contents = nullptr;
  gsize length = 0;
  bool found = false;
  bool chinese = false;
  if (g_file_get_contents(path, &contents, &length, nullptr) &&
      contents != nullptr) {
    const std::string body(contents, length);
    const std::string key = "\"language\"";
    const std::size_t pos = body.find(key);
    if (pos != std::string::npos) {
      const std::size_t colon = body.find(':', pos + key.size());
      const std::size_t quote =
          colon == std::string::npos ? std::string::npos
                                     : body.find('"', colon + 1);
      if (quote != std::string::npos) {
        const std::size_t end = body.find('"', quote + 1);
        if (end != std::string::npos) {
          const std::string language = body.substr(quote + 1, end - quote - 1);
          chinese = language.rfind("zh", 0) == 0;
          found = !language.empty();
        }
      }
    }
  }
  g_free(contents);
  g_free(path);
  if (found) return chinese;
  const char* lang = std::getenv("LANGUAGE");
  if (lang == nullptr || *lang == '\0') lang = std::getenv("LC_ALL");
  if (lang == nullptr || *lang == '\0') lang = std::getenv("LANG");
  return lang != nullptr && std::strncmp(lang, "zh", 2) == 0;
}

void apply_tray_language() {
  const bool zh = tray_uses_chinese();
  if (g_tray_show_item != nullptr) {
    gtk_menu_item_set_label(GTK_MENU_ITEM(g_tray_show_item),
                            zh ? "显示 moLink" : "Show moLink");
  }
  if (g_tray_quit_item != nullptr) {
    gtk_menu_item_set_label(GTK_MENU_ITEM(g_tray_quit_item),
                            zh ? "退出" : "Quit");
  }
}

void on_tray_activate(GtkStatusIcon*, gpointer data) {
  show_window(GTK_WIDGET(data));
}

void on_tray_popup(GtkStatusIcon* icon, guint button, guint time, gpointer) {
  if (g_tray_menu == nullptr) return;
  apply_tray_language();
  gtk_menu_popup(GTK_MENU(g_tray_menu), nullptr, nullptr,
                 gtk_status_icon_position_menu, icon, button, time);
}

void on_menu_show(GtkMenuItem*, gpointer data) { show_window(GTK_WIDGET(data)); }

void on_menu_quit(GtkMenuItem*, gpointer) { gtk_main_quit(); }

gboolean on_delete_event(GtkWidget* window, GdkEvent*, gpointer) {
  if (g_url_mode) return FALSE;
  hide_to_tray(window);
  return TRUE;
}

void setup_tray(GtkWidget* window) {
  const std::string icon = find_icon();
  if (!icon.empty()) {
    g_tray_icon = gtk_status_icon_new_from_file(icon.c_str());
  } else {
    g_tray_icon = gtk_status_icon_new_from_stock(GTK_STOCK_NETWORK);
  }
  gtk_status_icon_set_tooltip_text(g_tray_icon, "moLink");
  gtk_status_icon_set_title(g_tray_icon, "moLink");
  gtk_status_icon_set_visible(g_tray_icon, TRUE);

  g_tray_menu = gtk_menu_new();
  g_tray_show_item = gtk_menu_item_new_with_label("Show moLink");
  g_tray_quit_item = gtk_menu_item_new_with_label("Quit");
  gtk_menu_shell_append(GTK_MENU_SHELL(g_tray_menu), g_tray_show_item);
  gtk_menu_shell_append(GTK_MENU_SHELL(g_tray_menu), g_tray_quit_item);
  apply_tray_language();
  g_signal_connect(g_tray_show_item, "activate", G_CALLBACK(on_menu_show),
                   window);
  g_signal_connect(g_tray_quit_item, "activate", G_CALLBACK(on_menu_quit),
                   nullptr);
  gtk_widget_show_all(g_tray_menu);

  g_signal_connect(g_tray_icon, "activate", G_CALLBACK(on_tray_activate), window);
  g_signal_connect(g_tray_icon, "popup-menu", G_CALLBACK(on_tray_popup), window);
}

gboolean drag_out_cleanup_cb(gpointer data) {
  auto* state = static_cast<DragOutState*>(data);
  state->cleanup_source = 0;
  std::error_code ec;
  fs::remove_all(state->dir, ec);
  if (g_drag_out == state) g_drag_out = nullptr;
  delete state;
  return G_SOURCE_REMOVE;
}

void schedule_drag_out_cleanup(DragOutState* state, guint seconds) {
  if (state == nullptr || state->cleanup_source != 0) return;
  state->cleanup_source =
      g_timeout_add_seconds(seconds, drag_out_cleanup_cb, state);
}

/// Fills the selection for every format the drag source advertises.
void on_drag_data_get(GtkWidget*, GdkDragContext*, GtkSelectionData* data,
                      guint info, guint, gpointer) {
  DragOutState* state = g_drag_out;
  if (state == nullptr) return;
  if (info == 0) {
    std::vector<char*> uris;
    uris.reserve(state->uris.size() + 1);
    for (const std::string& uri : state->uris) {
      uris.push_back(const_cast<char*>(uri.c_str()));
    }
    uris.push_back(nullptr);
    gtk_selection_data_set_uris(data, uris.data());
  } else if (info == 2) {
    // Nautilus also understands this; a plain uri-list is the fallback.
    gtk_selection_data_set_text(data, state->copied.c_str(), -1);
  } else {
    gtk_selection_data_set_text(data, state->plain.c_str(), -1);
  }
}

gboolean on_drag_failed(GtkWidget*, GdkDragContext*, GtkDragResult, gpointer) {
  // A cancelled drag is not an error: keep the cache around long enough for
  // the user to try again, then let the cleanup timer remove it.
  schedule_drag_out_cleanup(g_drag_out, 300);
  return FALSE;
}

void on_drag_end(GtkWidget*, GdkDragContext* context, gpointer) {
  DragOutState* state = g_drag_out;
  if (state == nullptr) return;
  const GdkDragAction action = gdk_drag_context_get_selected_action(context);
  const bool copied =
      action == GDK_ACTION_COPY || action == GDK_ACTION_MOVE ||
      action == GDK_ACTION_LINK;
  schedule_drag_out_cleanup(state, copied ? 60 : 300);
}

/// Removes cache folders left behind by an earlier crash or a killed shell.
/// Only molink's own dragout root is touched.
void prune_stale_drag_out() {
  std::vector<std::string> roots;
  const char* cache = g_get_user_cache_dir();
  if (cache != nullptr && *cache != '\0') {
    roots.push_back((fs::path(cache) / "molink" / "dragout").string());
  }
  const char* home = g_get_home_dir();
  if (home != nullptr && *home != '\0') {
    roots.push_back((fs::path(home) / ".cache" / "molink" / "dragout").string());
  }
  const auto now = fs::file_time_type::clock::now();
  for (const std::string& root : roots) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) continue;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
      if (ec) break;
      std::error_code age_ec;
      const auto written = fs::last_write_time(entry.path(), age_ec);
      if (age_ec) continue;
      if (now - written > std::chrono::hours(1)) {
        std::error_code remove_ec;
        fs::remove_all(entry.path(), remove_ec);
      }
    }
  }
}

/// Parses `{"dir": "...", "paths": ["..."]}` from the bridge and starts the
/// GTK drag. The JSON is parsed with JSC so no extra dependency is needed.
void start_drag_out(JSCContext* context, const std::string& json) {
  if (context == nullptr || json.empty()) return;
  JSCValue* root = jsc_value_new_from_json(context, json.c_str());
  if (root == nullptr || !jsc_value_is_object(root)) {
    if (root != nullptr) g_object_unref(root);
    g_printerr("moLink shell: drag-out payload is not an object\n");
    return;
  }
  JSCValue* dir_value = jsc_value_object_get_property(root, "dir");
  JSCValue* paths_value = jsc_value_object_get_property(root, "paths");
  std::string dir;
  if (dir_value != nullptr && jsc_value_is_string(dir_value)) {
    char* text = jsc_value_to_string(dir_value);
    if (text != nullptr) dir = text;
    g_free(text);
  }
  std::vector<std::string> paths;
  if (paths_value != nullptr && jsc_value_is_array(paths_value)) {
    JSCValue* length_value =
        jsc_value_object_get_property(paths_value, "length");
    const int length = length_value != nullptr
                           ? jsc_value_to_int32(length_value)
                           : 0;
    if (length_value != nullptr) g_object_unref(length_value);
    for (int index = 0; index < length; ++index) {
      JSCValue* item = jsc_value_object_get_property_at_index(paths_value,
                                                              index);
      if (item == nullptr) continue;
      if (jsc_value_is_string(item)) {
        char* text = jsc_value_to_string(item);
        if (text != nullptr && *text != '\0') paths.emplace_back(text);
        g_free(text);
      }
      g_object_unref(item);
    }
  }
  if (paths_value != nullptr) g_object_unref(paths_value);
  if (dir_value != nullptr) g_object_unref(dir_value);
  g_object_unref(root);

  if (paths.empty()) {
    g_printerr("moLink shell: drag-out without files\n");
    return;
  }
  GtkWidget* window = g_main_window;
  if (window == nullptr) return;

  auto* state = new DragOutState();
  state->dir = dir;
  for (const std::string& path : paths) {
    GError* error = nullptr;
    gchar* uri = g_filename_to_uri(path.c_str(), nullptr, &error);
    if (uri == nullptr) {
      g_printerr("moLink shell: drag-out path %s: %s\n", path.c_str(),
                 error != nullptr ? error->message : "invalid path");
      if (error != nullptr) g_error_free(error);
      continue;
    }
    state->uris.emplace_back(uri);
    g_free(uri);
  }
  if (state->uris.empty()) {
    delete state;
    return;
  }
  for (std::size_t index = 0; index < state->uris.size(); ++index) {
    if (index != 0) {
      state->plain += "\n";
      state->copied += "\n";
    }
    state->plain += state->uris[index];
    if (index == 0) state->copied += "copy\n";
    state->copied += state->uris[index];
  }

  g_drag_out = state;

  GdkDisplay* display = gtk_widget_get_display(window);
  GdkSeat* seat = gdk_display_get_default_seat(display);
  GdkDevice* pointer = seat != nullptr ? gdk_seat_get_pointer(seat) : nullptr;
  gint pointer_x = 0;
  gint pointer_y = 0;
  if (pointer != nullptr) {
    gdk_device_get_position(pointer, nullptr, &pointer_x, &pointer_y);
  }
  gint origin_x = 0;
  gint origin_y = 0;
  GdkWindow* gdk_window = gtk_widget_get_window(window);
  if (gdk_window != nullptr) {
    gdk_window_get_origin(gdk_window, &origin_x, &origin_y);
  }

  GdkDragContext* drag = gtk_drag_begin_with_coordinates(
      window, gtk_drag_source_get_target_list(window), GDK_ACTION_COPY, 1,
      nullptr, pointer_x - origin_x, pointer_y - origin_y);
  if (drag == nullptr) {
    g_printerr("moLink shell: drag-out did not start\n");
    schedule_drag_out_cleanup(state, 300);
    return;
  }

  GdkPixbuf* icon = gdk_pixbuf_new_from_file_at_size(paths[0].c_str(), 48, 48,
                                                     nullptr);
  if (icon != nullptr) {
    gtk_drag_set_icon_pixbuf(drag, icon, 8, 8);
    g_object_unref(icon);
  } else {
    const gboolean is_dir =
        g_file_test(paths[0].c_str(), G_FILE_TEST_IS_DIR) != FALSE;
    gtk_drag_set_icon_name(drag, is_dir ? "folder" : "text-x-generic", 0, 0);
  }
  // drag-data-get lives on the source widget (connected once in main), not on
  // the GdkDragContext; connecting it here is a GLib critical and never fires.
  g_printerr("moLink shell: drag-out started with %zu file(s)\n",
             state->uris.size());
}

/// Opens the same directory the core writes diagnostics into. The page only
/// sends the command name, never a path: resolving it here keeps a remote or
/// compromised page from asking the shell to open an arbitrary location.
void open_log_directory(GtkWidget* window) {
  std::string dir;
  const char* xdg = std::getenv("XDG_DATA_HOME");
  const char* home = std::getenv("HOME");
  if (xdg != nullptr && xdg[0] != '\0') {
    dir = std::string(xdg) + "/molink/logs";
  } else if (home != nullptr && home[0] != '\0') {
    dir = std::string(home) + "/.local/share/molink/logs";
  } else {
    dir = "/tmp/molink/logs";
  }
  std::error_code ec;
  fs::create_directories(dir, ec);
  gchar* uri = g_filename_to_uri(dir.c_str(), nullptr, nullptr);
  if (uri != nullptr) {
    gtk_show_uri_on_window(GTK_WINDOW(window), uri, GDK_CURRENT_TIME, nullptr);
    g_free(uri);
  }
}

/// JSON-quotes an arbitrary filesystem path for an evaluate_javascript()
/// payload. A path may contain quotes, backslashes or control characters;
/// building the script with raw %s would break the page's JS.
std::string json_string(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 2);
  out.push_back('"');
  for (const unsigned char ch : text) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (ch < 0x20) {
          char escaped[8];
          std::snprintf(escaped, sizeof escaped, "\\u%04x", ch);
          out += escaped;
        } else {
          out.push_back(static_cast<char>(ch));
        }
    }
  }
  out.push_back('"');
  return out;
}

/// Sends the folder the native chooser picked back to the page. Cancelling
/// dispatches nothing, so the page never sees an empty path.
gboolean on_directory_chosen(GtkDialog* dialog, gint response, gpointer) {
  if (response == GTK_RESPONSE_ACCEPT) {
    gchar* folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    if (folder != nullptr && *folder != '\0') {
      const std::string quoted = json_string(folder);
      gchar* script = g_strdup_printf(
          "window.dispatchEvent(new CustomEvent('molink-directory',"
          "{detail:%s}))",
          quoted.c_str());
      shell_eval_js(script);
      g_free(script);
    }
    g_free(folder);
  }
  gtk_widget_destroy(GTK_WIDGET(dialog));
  return FALSE;
}

/// WebKitGTK has neither the File System Access API nor a working
/// `<input webkitdirectory>` (it degrades to a file dialog). The shell owns a
/// real GTK folder chooser instead; the chosen absolute path goes back as the
/// `molink-directory` DOM event.
void pick_directory(GtkWidget* window) {
  if (window == nullptr) return;
  // An active X11 keyboard grab would keep the modal dialog from getting
  // key events; release it first (focus loss would do this anyway).
  release_keyboard_grab("directory dialog");
  GtkWidget* dialog = gtk_file_chooser_dialog_new(
      "选择目录", GTK_WINDOW(window), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
      "取消", GTK_RESPONSE_CANCEL, "选择", GTK_RESPONSE_ACCEPT, nullptr);
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(window));
  const char* home = g_get_home_dir();
  if (home != nullptr && *home != '\0') {
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), home);
  }
  g_signal_connect(dialog, "response", G_CALLBACK(on_directory_chosen),
                   nullptr);
  gtk_widget_show(dialog);
}

void on_script_message(WebKitUserContentManager*, WebKitJavascriptResult* result,
                       gpointer data) {
  GtkWidget* window = GTK_WIDGET(data);
  JSCValue* value = webkit_javascript_result_get_js_value(result);
  if (value == nullptr || !jsc_value_is_string(value)) return;
  char* raw = jsc_value_to_string(value);
  const std::string action = raw != nullptr ? raw : "";
  g_free(raw);
  if (action == "grab-keyboard") {
    grab_keyboard(window);
  } else if (action == "release-keyboard") {
    g_grab_requested = false;
    release_keyboard_grab("page request");
  } else if (action == "pick-directory") {
    pick_directory(window);
  } else if (action == "minimize") {
    gtk_window_iconify(GTK_WINDOW(window));
  } else if (action == "maximize") {
    if (gtk_window_is_maximized(GTK_WINDOW(window))) {
      gtk_window_unmaximize(GTK_WINDOW(window));
    } else {
      gtk_window_maximize(GTK_WINDOW(window));
    }
  } else if (action == "fullscreen-on") {
    gtk_window_fullscreen(GTK_WINDOW(window));
  } else if (action == "fullscreen-off") {
    gtk_window_unfullscreen(GTK_WINDOW(window));
  } else if (action == "fullscreen") {
    GdkWindow* gdk_window = gtk_widget_get_window(window);
    const bool fullscreen =
        gdk_window != nullptr &&
        (gdk_window_get_state(gdk_window) & GDK_WINDOW_STATE_FULLSCREEN) != 0;
    if (fullscreen) {
      gtk_window_unfullscreen(GTK_WINDOW(window));
    } else {
      gtk_window_fullscreen(GTK_WINDOW(window));
    }
  } else if (action.rfind("open-url:", 0) == 0 ||
             action.rfind("update-url:", 0) == 0) {
    // Linux packages need the user's package manager, so both actions open
    // the download in the system browser.
    const std::size_t colon = action.find(':');
    const std::string url = action.substr(colon + 1);
    if (!url.empty()) {
      gtk_show_uri_on_window(GTK_WINDOW(window), url.c_str(),
                             GDK_CURRENT_TIME, nullptr);
    }
  } else if (action == "open-log-dir") {
    open_log_directory(window);
  } else if (action == "close") {
    if (g_url_mode) {
      gtk_widget_destroy(window);
    } else {
      hide_to_tray(window);
    }
  } else if (action == "drag") {
    GdkDisplay* display = gtk_widget_get_display(window);
    GdkSeat* seat = gdk_display_get_default_seat(display);
    GdkDevice* pointer = gdk_seat_get_pointer(seat);
    gint x = 0;
    gint y = 0;
    gdk_device_get_position(pointer, nullptr, &x, &y);
    gtk_window_begin_move_drag(GTK_WINDOW(window), 1, x, y, GDK_CURRENT_TIME);
  } else if (action == "resize-top" || action == "resize-bottom" ||
             action == "resize-left" || action == "resize-right" ||
             action == "resize-tl" || action == "resize-tr" ||
             action == "resize-bl" || action == "resize-br") {
    GdkWindowEdge edge = GDK_WINDOW_EDGE_NORTH_WEST;
    if (action == "resize-top") edge = GDK_WINDOW_EDGE_NORTH;
    else if (action == "resize-bottom") edge = GDK_WINDOW_EDGE_SOUTH;
    else if (action == "resize-left") edge = GDK_WINDOW_EDGE_WEST;
    else if (action == "resize-right") edge = GDK_WINDOW_EDGE_EAST;
    else if (action == "resize-tr") edge = GDK_WINDOW_EDGE_NORTH_EAST;
    else if (action == "resize-bl") edge = GDK_WINDOW_EDGE_SOUTH_WEST;
    else if (action == "resize-br") edge = GDK_WINDOW_EDGE_SOUTH_EAST;
    GdkDisplay* display = gtk_widget_get_display(window);
    GdkSeat* seat = gdk_display_get_default_seat(display);
    GdkDevice* pointer = gdk_seat_get_pointer(seat);
    gint x = 0;
    gint y = 0;
    gdk_device_get_position(pointer, nullptr, &x, &y);
    gtk_window_begin_resize_drag(GTK_WINDOW(window), edge, 1, x, y,
                                 GDK_CURRENT_TIME);
  } else if (action.rfind("drag-out:", 0) == 0) {
    start_drag_out(jsc_value_get_context(value), action.substr(9));
  } else if (action == "quit") {
    gtk_main_quit();
  }
}

/// Decides where a started download lands: the user's Downloads folder, with
/// the name the page asked for. WebKit emits this per download after the
/// context's download-started, and its suggested_filename is the only place
/// the <a download> name survives for the drawer's blob: URLs (their URI
/// response carries no filename). Without a handler WebKit silently
/// overwrites an existing file of the same name.
void on_download_decide_destination(WebKitDownload* download,
                                    const gchar* suggested, gpointer) {
  // The name comes from the remote machine, so keep only its last component:
  // a name like "../../x" must not escape the Downloads folder.
  gchar* base = g_path_get_basename(suggested != nullptr && *suggested != '\0'
                                        ? suggested
                                        : "molink-download");
  // XDG may name a Downloads folder that does not exist; WebKit refuses a
  // destination whose parent is missing, so fall back to the home directory.
  const gchar* downloads = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
  if (downloads == nullptr || *downloads == '\0' ||
      !g_file_test(downloads, G_FILE_TEST_IS_DIR)) {
    downloads = g_get_home_dir();
  }
  gchar* dir = g_strdup(downloads != nullptr ? downloads : "/tmp");
  // Number collisions before the extension ("notes (1).txt"), like a browser.
  gchar* stem = g_strdup(base);
  gchar* extension = nullptr;
  gchar* dot = g_strrstr(stem, ".");
  if (dot != nullptr && dot != stem && dot[1] != '\0') {
    *dot = '\0';
    extension = dot + 1;
  }
  gchar* dest = g_build_filename(dir, base, nullptr);
  for (int suffix = 1; suffix < 1000 && g_file_test(dest, G_FILE_TEST_EXISTS);
       ++suffix) {
    g_free(dest);
    gchar* numbered = extension != nullptr
                          ? g_strdup_printf("%s (%d).%s", stem, suffix, extension)
                          : g_strdup_printf("%s (%d)", stem, suffix);
    dest = g_build_filename(dir, numbered, nullptr);
    g_free(numbered);
  }
  g_free(stem);
  webkit_download_set_destination(download, dest);
  std::fprintf(stderr, "download -> %s\n", dest);
  g_free(dest);
  g_free(base);
  g_free(dir);
}

/// The context reports every download here; hook the per-download signal that
/// carries the suggested filename.
void on_download_started(WebKitWebContext*, WebKitDownload* download, gpointer) {
  g_signal_connect(download, "decide-destination",
                   G_CALLBACK(on_download_decide_destination), nullptr);
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--smoke-test") == 0) {
      g_smoke = true;
    } else if (std::strcmp(argv[i], "--url") == 0 && i + 1 < argc) {
      g_url_override = argv[++i];
    } else if (std::strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
      g_core_host = argv[++i];
    } else if (std::strcmp(argv[i], "--surface") == 0 && i + 1 < argc) {
      g_surface_path = argv[++i];
    }
  }
  g_url_mode = !g_url_override.empty();

  if (!g_url_mode && !g_smoke && g_surface_path.empty()) {
    const std::string instance_path = instance_socket_path();
    bool already_running = false;
    if (!claim_instance_socket(instance_path, already_running)) {
      if (already_running) return 0;
      std::fprintf(stderr,
                   "moLink shell: could not create the single-instance socket "
                   "at %s; starting anyway\n",
                   instance_path.c_str());
    } else {
      std::atexit(cleanup_instance_socket);
    }
  }

  gtk_init(&argc, &argv);

  if (!g_surface_path.empty()) {
    // A native picture window for the core's frame surface: no WebView is
    // involved in the picture path at all.
    return run_surface_window(g_surface_path);
  }

  std::string url;
  if (g_url_mode) {
    url = g_url_override;
  } else {
    const std::string core = find_core();
    if (core.empty()) {
      std::fprintf(stderr,
                   "moLink shell: molink-core not found; set MOLINK_CORE or "
                   "build backend/\n");
      return 1;
    }
    const std::string web_root = find_web_root();
    if (web_root.empty()) {
      std::fprintf(stderr,
                   "moLink shell: frontend/dist not found; run pnpm build in "
                   "frontend/\n");
      return 1;
    }

    int port = 0;
    std::string token;
    std::string client_token;
    std::string surface_path;
    bool tls = false;
    std::string error;
    if (!start_core(core, web_root, port, token, client_token, tls,
                    surface_path, error)) {
      std::fprintf(stderr, "moLink shell: %s\n", error.c_str());
      stop_core();
      return 1;
    }

    g_core_tls = tls;
    const std::string host = ui_host();
    url = std::string(g_core_tls ? "https://" : "http://") + url_host(host) +
          ":" + std::to_string(port) + "/?host=";
    url += host + "&port=" + std::to_string(port);
    // The core is publishing to a native surface: open the window that draws
    // it, as a sibling process so the WebView keeps owning the UI.
    if (!surface_path.empty()) {
      const std::string executable = exe_path();
      if (!executable.empty()) {
        char* child_argv[] = {const_cast<char*>(executable.c_str()),
                              const_cast<char*>("--surface"),
                              const_cast<char*>(surface_path.c_str()), nullptr};
        g_spawn_async(nullptr, child_argv, nullptr,
                      static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH), nullptr,
                      nullptr, nullptr, nullptr);
      }
    }
    url += "&shell=1&platform=linux";
    if (!client_token.empty()) url += "&client=" + client_token;
  }

  GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  g_main_window = window;
  gtk_window_set_title(GTK_WINDOW(window), "moLink");
  gtk_window_set_default_size(GTK_WINDOW(window), 1280, 820);
  gtk_window_set_resizable(GTK_WINDOW(window), TRUE);
  gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
  gtk_widget_set_app_paintable(window, TRUE);

  GdkScreen* screen = gtk_widget_get_screen(window);
  GdkVisual* rgba_visual = gdk_screen_get_rgba_visual(screen);
  if (rgba_visual != nullptr && gdk_screen_is_composited(screen)) {
    gtk_widget_set_visual(window, rgba_visual);
  }
  const std::string icon_path = find_icon();
  if (!icon_path.empty()) {
    gtk_window_set_icon_from_file(GTK_WINDOW(window), icon_path.c_str(), nullptr);
  }

  GtkWidget* view = webkit_web_view_new();
  g_web_view = WEBKIT_WEB_VIEW(view);
  WebKitSettings* settings = webkit_web_view_get_settings(WEBKIT_WEB_VIEW(view));
  webkit_settings_set_enable_developer_extras(settings, TRUE);
  webkit_settings_set_enable_fullscreen(settings, TRUE);
  webkit_settings_set_javascript_can_access_clipboard(settings, TRUE);
  webkit_settings_set_javascript_can_open_windows_automatically(settings, TRUE);
  // getUserMedia for voice calls. The permission handler above only allows
  // requests from the core's origin; WebRTC carries the capture pipeline on
  // newer WebKitGTK releases.
  webkit_settings_set_enable_media_stream(settings, TRUE);
  webkit_settings_set_enable_webrtc(settings, TRUE);
  // In the GTK3 (4.1) API the download signal lives on the context, not the
  // view; a view connection is silently invalid and leaves WebKit's default
  // handler in charge.
  g_signal_connect(webkit_web_view_get_context(WEBKIT_WEB_VIEW(view)),
                   "download-started", G_CALLBACK(on_download_started), nullptr);
  gtk_container_add(GTK_CONTAINER(window), view);

  static const std::string origin =
      std::string(g_core_tls ? "https://" : "http://") +
      url_host(ui_host()) + ":";
  g_signal_connect(view, "permission-request",
                   G_CALLBACK(on_permission_request),
                   const_cast<char*>(origin.c_str()));
  g_signal_connect(view, "load-failed-with-tls-errors",
                   G_CALLBACK(on_tls_error), nullptr);
  g_signal_connect(view, "create", G_CALLBACK(on_create_web_view), nullptr);
  g_signal_connect(view, "load-changed", G_CALLBACK(on_load_changed), nullptr);
  g_signal_connect(view, "load-failed", G_CALLBACK(on_load_failed), nullptr);
  g_signal_connect(window, "key-press-event", G_CALLBACK(on_key_press), nullptr);
  g_signal_connect(window, "destroy", G_CALLBACK(on_destroy), nullptr);
  g_signal_connect(window, "delete-event", G_CALLBACK(on_delete_event), nullptr);
  g_signal_connect(window, "size-allocate", G_CALLBACK(on_size_allocate), nullptr);
  g_signal_connect(window, "window-state-event", G_CALLBACK(on_window_state),
                   nullptr);
  // The grab must not survive focus loss, unmapping (hide-to-tray) or a
  // minimized window; each of these is an independent escape hatch.
  g_signal_connect(window, "focus-out-event", G_CALLBACK(on_grab_focus_out),
                   nullptr);
  g_signal_connect(window, "focus-in-event", G_CALLBACK(on_grab_focus_in),
                   nullptr);
  g_signal_connect(window, "unmap", G_CALLBACK(on_grab_unmap), nullptr);
  // Ctrl+Alt+Shift+Escape is checked before WebKit sees the key, so the page
  // never forwards a lone Escape release to the remote.
  gtk_key_snooper_install(on_grab_key_snooper, window);

  WebKitUserContentManager* content_manager =
      webkit_web_view_get_user_content_manager(WEBKIT_WEB_VIEW(view));
  webkit_user_content_manager_register_script_message_handler(content_manager,
                                                              "molink");
  g_signal_connect(content_manager, "script-message-received::molink",
                   G_CALLBACK(on_script_message), window);

  // The page starts this drag by name (`drag-out:`); GTK still needs the
  // window registered as a source so the data-get/drag-end signals fire.
  static GtkTargetEntry drag_targets[] = {
      {const_cast<gchar*>("text/uri-list"), 0, 0},
      {const_cast<gchar*>("text/plain;charset=utf-8"), 0, 1},
      {const_cast<gchar*>("x-special/gnome-copied-files"), 0, 2},
  };
  gtk_drag_source_set(window, GDK_BUTTON1_MASK, drag_targets,
                      G_N_ELEMENTS(drag_targets), GDK_ACTION_COPY);
  g_signal_connect(window, "drag-data-get", G_CALLBACK(on_drag_data_get),
                   nullptr);
  g_signal_connect(window, "drag-failed", G_CALLBACK(on_drag_failed), nullptr);
  g_signal_connect(window, "drag-end", G_CALLBACK(on_drag_end), nullptr);
  prune_stale_drag_out();

  if (!g_url_mode) setup_tray(window);

  GtkCssProvider* css = gtk_css_provider_new();
  gtk_css_provider_load_from_data(
      css,
      "window, .background { background-color: transparent; }"
      "webkitwebview { background-color: #0f1719; }",
      -1, nullptr);
  gtk_style_context_add_provider_for_screen(
      gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  webkit_web_view_load_uri(WEBKIT_WEB_VIEW(view), url.c_str());
  gtk_widget_show_all(window);
  apply_window_shape(window);
  // Safety net for the paths that never deliver another size-allocate to the
  // WebView (notably a window the WM restores maximized at startup). The
  // check is a cheap no-op when the allocation already matches.
  g_timeout_add(500,
                [](gpointer data) -> gboolean {
                  GtkWidget* window = GTK_WIDGET(data);
                  // The rounded-corner shape region is sized when it is
                  // applied. A window the WM maximizes at startup can keep the
                  // region from its default size, clipping the maximized
                  // window even though the WebView itself is correct. The GDK
                  // state is authoritative here, unlike the event flags.
                  GdkWindow* gdk_window = gtk_widget_get_window(window);
                  if (gdk_window != nullptr) {
                    const GdkWindowState state =
                        gdk_window_get_state(gdk_window);
                    if ((state & GDK_WINDOW_STATE_MAXIMIZED) != 0 ||
                        (state & GDK_WINDOW_STATE_FULLSCREEN) != 0) {
                      apply_square_shape(window);
                    }
                  }
                  force_child_allocate(window);
                  return G_SOURCE_CONTINUE;
                },
                window);
  g_timeout_add_seconds(10, log_viewport, view);

  if (!g_url_mode) g_timeout_add_seconds(1, check_core, nullptr);
  if (g_smoke) g_timeout_add_seconds(15, smoke_timeout, nullptr);
  g_unix_signal_add(SIGTERM, on_quit_signal, nullptr);
  g_unix_signal_add(SIGINT, on_quit_signal, nullptr);

  gtk_main();
  stop_core();
  return g_exit_code;
}
