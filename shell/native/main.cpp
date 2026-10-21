// moLink desktop shell — GTK 3 + WebKitGTK 4.1.
//
// Starts molink-core, reads its ready line, then loads the UI that the core
// serves at "/" into a system WebView. No Rust, no bundled browser engine.

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <webkit2/webkit2.h>
#include <cairo.h>
#include <jsc/jsc.h>

#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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

bool start_core(const std::string& core, const std::string& web_root, int& port,
                std::string& token, std::string& err) {
  std::vector<std::string> args = {core, "--port", "0", "--print-ready",
                                   "--parent-pid", std::to_string(getpid())};
  if (!web_root.empty()) {
    args.push_back("--web-root");
    args.push_back(web_root);
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
  while (!complete && std::chrono::steady_clock::now() < deadline) {
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
    if (line.find('\n') != std::string::npos) complete = true;
  }
  if (stdout_fd >= 0) ::close(stdout_fd);

  long parsed_port = 0;
  if (!complete || !json_number(line, "port", parsed_port) ||
      !json_string(line, "token", token) || parsed_port <= 0) {
    err = "molink-core did not report a ready port and token";
    return false;
  }
  port = static_cast<int>(parsed_port);
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

void on_destroy(GtkWidget*, gpointer) { gtk_main_quit(); }

void on_load_changed(WebKitWebView* view, WebKitLoadEvent event, gpointer) {
  if (event != WEBKIT_LOAD_FINISHED) return;
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
bool g_url_mode = false;
std::string g_url_override;

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

void apply_window_shape(GtkWidget* widget) {
  if (!gtk_widget_get_realized(widget)) return;
  if (gtk_window_is_maximized(GTK_WINDOW(widget))) {
    gtk_widget_shape_combine_region(widget, nullptr);
    return;
  }
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  if (allocation.width <= 0 || allocation.height <= 0) return;
  cairo_surface_t* surface = cairo_image_surface_create(
      CAIRO_FORMAT_A1, allocation.width, allocation.height);
  cairo_t* cr = cairo_create(surface);
  rounded_rect_path(cr, 0, 0, allocation.width, allocation.height, 14.0);
  cairo_fill(cr);
  cairo_destroy(cr);
  cairo_region_t* region = gdk_cairo_region_create_from_surface(surface);
  cairo_surface_destroy(surface);
  if (region != nullptr) {
    gtk_widget_shape_combine_region(widget, region);
    cairo_region_destroy(region);
  }
}

gboolean on_size_allocate(GtkWidget* widget, GdkRectangle*, gpointer) {
  apply_window_shape(widget);
  return FALSE;
}

gboolean on_window_state(GtkWidget* widget, GdkEventWindowState* event,
                         gpointer) {
  if ((event->changed_mask &
       (GDK_WINDOW_STATE_MAXIMIZED | GDK_WINDOW_STATE_FULLSCREEN)) != 0) {
    apply_window_shape(widget);
  }
  return FALSE;
}

void show_window(GtkWidget* window) {
  gtk_widget_show(window);
  gtk_window_deiconify(GTK_WINDOW(window));
  gtk_window_present(GTK_WINDOW(window));
}

void hide_to_tray(GtkWidget* window) {
  gtk_widget_hide(window);
  if (g_tray_icon != nullptr) gtk_status_icon_set_visible(g_tray_icon, TRUE);
}

void on_tray_activate(GtkStatusIcon*, gpointer data) {
  show_window(GTK_WIDGET(data));
}

void on_tray_popup(GtkStatusIcon* icon, guint button, guint time, gpointer) {
  if (g_tray_menu == nullptr) return;
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
  GtkWidget* show_item = gtk_menu_item_new_with_label("Show moLink");
  GtkWidget* quit_item = gtk_menu_item_new_with_label("Quit");
  gtk_menu_shell_append(GTK_MENU_SHELL(g_tray_menu), show_item);
  gtk_menu_shell_append(GTK_MENU_SHELL(g_tray_menu), quit_item);
  g_signal_connect(show_item, "activate", G_CALLBACK(on_menu_show), window);
  g_signal_connect(quit_item, "activate", G_CALLBACK(on_menu_quit), nullptr);
  gtk_widget_show_all(g_tray_menu);

  g_signal_connect(g_tray_icon, "activate", G_CALLBACK(on_tray_activate), window);
  g_signal_connect(g_tray_icon, "popup-menu", G_CALLBACK(on_tray_popup), window);
}

void on_script_message(WebKitUserContentManager*, WebKitJavascriptResult* result,
                       gpointer data) {
  GtkWidget* window = GTK_WIDGET(data);
  JSCValue* value = webkit_javascript_result_get_js_value(result);
  if (value == nullptr || !jsc_value_is_string(value)) return;
  char* raw = jsc_value_to_string(value);
  const std::string action = raw != nullptr ? raw : "";
  g_free(raw);
  if (action == "minimize") {
    gtk_window_iconify(GTK_WINDOW(window));
  } else if (action == "maximize") {
    if (gtk_window_is_maximized(GTK_WINDOW(window))) {
      gtk_window_unmaximize(GTK_WINDOW(window));
    } else {
      gtk_window_maximize(GTK_WINDOW(window));
    }
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
  } else if (action == "quit") {
    gtk_main_quit();
  }
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--smoke-test") == 0) {
      g_smoke = true;
    } else if (std::strcmp(argv[i], "--url") == 0 && i + 1 < argc) {
      g_url_override = argv[++i];
    }
  }
  g_url_mode = !g_url_override.empty();

  gtk_init(&argc, &argv);

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
    std::string error;
    if (!start_core(core, web_root, port, token, error)) {
      std::fprintf(stderr, "moLink shell: %s\n", error.c_str());
      stop_core();
      return 1;
    }

    url = "http://127.0.0.1:" + std::to_string(port) + "/?host=";
    url += "127.0.0.1&port=" + std::to_string(port);
    url += "&shell=1&platform=linux";
  }

  GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
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
  WebKitSettings* settings = webkit_web_view_get_settings(WEBKIT_WEB_VIEW(view));
  webkit_settings_set_enable_developer_extras(settings, TRUE);
  webkit_settings_set_javascript_can_access_clipboard(settings, TRUE);
  webkit_settings_set_javascript_can_open_windows_automatically(settings, TRUE);
  gtk_container_add(GTK_CONTAINER(window), view);

  static const char kOrigin[] = "http://127.0.0.1:";
  g_signal_connect(view, "permission-request",
                   G_CALLBACK(on_permission_request),
                   const_cast<char*>(kOrigin));
  g_signal_connect(view, "create", G_CALLBACK(on_create_web_view), nullptr);
  g_signal_connect(view, "load-changed", G_CALLBACK(on_load_changed), nullptr);
  g_signal_connect(view, "load-failed", G_CALLBACK(on_load_failed), nullptr);
  g_signal_connect(window, "key-press-event", G_CALLBACK(on_key_press), nullptr);
  g_signal_connect(window, "destroy", G_CALLBACK(on_destroy), nullptr);
  g_signal_connect(window, "delete-event", G_CALLBACK(on_delete_event), nullptr);
  g_signal_connect(window, "size-allocate", G_CALLBACK(on_size_allocate), nullptr);
  g_signal_connect(window, "window-state-event", G_CALLBACK(on_window_state),
                   nullptr);

  WebKitUserContentManager* content_manager =
      webkit_web_view_get_user_content_manager(WEBKIT_WEB_VIEW(view));
  webkit_user_content_manager_register_script_message_handler(content_manager,
                                                              "molink");
  g_signal_connect(content_manager, "script-message-received::molink",
                   G_CALLBACK(on_script_message), window);

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

  if (!g_url_mode) g_timeout_add_seconds(1, check_core, nullptr);
  if (g_smoke) g_timeout_add_seconds(15, smoke_timeout, nullptr);

  gtk_main();
  stop_core();
  return g_exit_code;
}
