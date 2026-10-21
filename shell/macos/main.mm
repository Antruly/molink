// moLink desktop shell for macOS — Cocoa + WKWebView.
//
// Starts molink-core, reads its ready line, then loads the UI served by the
// core. The .app bundle keeps the core and its dylibs in Contents/MacOS and the
// frontend in Contents/Resources/web.

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include <dispatch/dispatch.h>
#include <mach-o/dyld.h>
#include <poll.h>
#include <spawn.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace {

namespace fs = std::filesystem;

pid_t g_core_pid = -1;
bool g_smoke = false;
bool g_smoke_done = false;
bool g_url_mode = false;
std::string g_url_override;

// Single-instance guard (normal launches only). The first shell listens on a
// per-user Unix socket; later launches send "show" and exit.
int g_instance_fd = -1;
std::string g_instance_path;

std::string exe_dir() {
  char buffer[4096];
  uint32_t size = sizeof(buffer);
  if (_NSGetExecutablePath(buffer, &size) != 0) return std::string();
  std::error_code ec;
  const fs::path resolved = fs::weakly_canonical(buffer, ec);
  return resolved.parent_path().string();
}

std::string first_existing(const std::vector<fs::path>& candidates) {
  std::error_code ec;
  for (const fs::path& candidate : candidates) {
    if (fs::is_regular_file(candidate, ec)) {
      return candidate.lexically_normal().string();
    }
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
    candidates.push_back(fs::path(dir) / "../Resources/molink-core");
    candidates.push_back(fs::path(dir) / "../../../backend/build/molink-core");
  }
  candidates.push_back("backend/build/molink-core");
  return first_existing(candidates);
}

std::string find_web_root() {
  if (const char* env = std::getenv("MOLINK_WEB_ROOT")) {
    if (fs::is_regular_file(fs::path(env) / "index.html")) return env;
  }
  const std::string dir = exe_dir();
  std::vector<fs::path> candidates;
  if (!dir.empty()) {
    candidates.push_back(fs::path(dir) / "../Resources/web");
    candidates.push_back(fs::path(dir) / "web");
    candidates.push_back(fs::path(dir) / "../../../frontend/dist");
  }
  candidates.push_back("frontend/dist");
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
                std::string& token, std::string& client_token,
                std::string& error) {
  int fds[2];
  if (pipe(fds) != 0) {
    error = "pipe failed";
    return false;
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&actions, fds[0]);
  posix_spawn_file_actions_addclose(&actions, fds[1]);

  std::vector<std::string> args = {core, "--print-ready", "--parent-pid",
                                   std::to_string(getpid())};
  if (!web_root.empty()) {
    args.push_back("--web-root");
    args.push_back(web_root);
  }
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (std::string& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
  argv.push_back(nullptr);

  const int rc = posix_spawn(&g_core_pid, core.c_str(), &actions, nullptr,
                             argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  ::close(fds[1]);
  if (rc != 0) {
    ::close(fds[0]);
    g_core_pid = -1;
    error = "could not start molink-core";
    return false;
  }

  std::string line;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  bool complete = false;
  while (!complete && std::chrono::steady_clock::now() < deadline) {
    struct pollfd descriptor;
    descriptor.fd = fds[0];
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    if (::poll(&descriptor, 1, 200) <= 0) continue;
    char buffer[4096];
    const ssize_t got = ::read(fds[0], buffer, sizeof(buffer));
    if (got <= 0) break;
    line.append(buffer, static_cast<std::size_t>(got));
    if (line.find('\n') != std::string::npos) complete = true;
  }
  ::close(fds[0]);

  long parsed_port = 0;
  if (!complete || !json_number(line, "port", parsed_port) ||
      !json_string(line, "token", token) ||
      !json_string(line, "client_token", client_token) || parsed_port <= 0) {
    error = "molink-core did not report a ready port and tokens";
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
    if (::waitpid(g_core_pid, &status, WNOHANG) == g_core_pid) {
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

void spawn_self(const std::string& url) {
  NSString* executable = [[NSBundle mainBundle] executablePath];
  std::string path = executable != nil ? [executable UTF8String]
                                       : exe_dir() + "/moLink";
  NSTask* task = [[NSTask alloc] init];
  task.executableURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
  task.arguments = @[ @"--url", [NSString stringWithUTF8String:url.c_str()] ];
  NSError* error = nil;
  [task launchAndReturnError:&error];
}

void fail(const char* message) {
  std::fprintf(stderr, "moLink shell: %s\n", message);
  if (g_smoke) {
    std::printf("SHELL_SMOKE_FAIL %s\n", message);
    std::fflush(stdout);
    g_smoke_done = true;
  }
  stop_core();
  std::exit(1);
}

}  // namespace

@interface AppDelegate
    : NSObject <NSApplicationDelegate, WKNavigationDelegate, WKUIDelegate,
                WKScriptMessageHandler, NSWindowDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) WKWebView* webView;
@property(nonatomic, copy) NSString* url;
- (void)showWindow;
@end

static AppDelegate* g_app_delegate = nil;

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
  (void)notification;

  NSRect frame = NSMakeRect(0, 0, 1280, 800);
  self.window = [[NSWindow alloc]
      initWithContentRect:frame
                styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                           NSWindowStyleMaskResizable |
                           NSWindowStyleMaskMiniaturizable)
                  backing:NSBackingStoreBuffered
                    defer:NO];
  self.window.title = @"moLink";
  self.window.titleVisibility = NSWindowTitleHidden;
  self.window.titlebarAppearsTransparent = YES;
  self.window.backgroundColor = [NSColor windowBackgroundColor];
  self.window.delegate = self;

  WKWebViewConfiguration* configuration =
      [[WKWebViewConfiguration alloc] init];
  WKUserContentController* controller = [[WKUserContentController alloc] init];
  [controller addScriptMessageHandler:self name:@"molink"];
  configuration.userContentController = controller;
  configuration.preferences.javaScriptCanOpenWindowsAutomatically = YES;

  self.webView = [[WKWebView alloc] initWithFrame:frame
                                    configuration:configuration];
  self.webView.navigationDelegate = self;
  self.webView.UIDelegate = self;
  self.webView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  [self.window.contentView addSubview:self.webView];

  [self.window center];
  [self.window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];

  NSURL* url = [NSURL URLWithString:self.url];
  [self.webView loadRequest:[NSURLRequest requestWithURL:url]];

  if (g_smoke) {
    dispatch_after(
        dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC),
        dispatch_get_main_queue(), ^{
          if (!g_smoke_done) {
            std::printf("SHELL_SMOKE_FAIL timeout\n");
            std::fflush(stdout);
            g_smoke_done = true;
            stop_core();
            std::exit(1);
          }
        });
  }
}

- (void)webView:(WKWebView*)webView
    didFinishNavigation:(WKNavigation*)navigation {
  (void)webView;
  (void)navigation;
  if (!g_smoke) return;
  const char* title = [[self.webView title] UTF8String];
  std::printf("SHELL_SMOKE_OK title=%s\n", title != nullptr ? title : "");
  std::fflush(stdout);
  g_smoke_done = true;
  stop_core();
  [NSApp terminate:nil];
}

- (void)webView:(WKWebView*)webView
    didFailProvisionalNavigation:(WKNavigation*)navigation
                       withError:(NSError*)error {
  (void)webView;
  (void)navigation;
  fail([[error localizedDescription] UTF8String]);
}

- (void)webView:(WKWebView*)webView
    didFailNavigation:(WKNavigation*)navigation
            withError:(NSError*)error {
  (void)webView;
  (void)navigation;
  fail([[error localizedDescription] UTF8String]);
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:
    (NSApplication*)sender {
  (void)sender;
  return g_url_mode ? YES : NO;
}

- (void)showWindow {
  [self.window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldHandleReopen:(NSApplication*)sender
                    hasVisibleWindows:(BOOL)flag {
  (void)sender;
  (void)flag;
  [self showWindow];
  return YES;
}

- (BOOL)windowShouldClose:(NSWindow*)sender {
  if (g_url_mode) {
    stop_core();
    return YES;
  }
  [sender orderOut:nil];
  return NO;
}

- (void)userContentController:(WKUserContentController*)controller
      didReceiveScriptMessage:(WKScriptMessage*)message {
  (void)controller;
  NSString* action = message.body;
  if (![action isKindOfClass:[NSString class]]) return;
  if ([action isEqualToString:@"minimize"]) {
    [self.window miniaturize:nil];
  } else if ([action isEqualToString:@"maximize"]) {
    [self.window zoom:nil];
  } else if ([action isEqualToString:@"fullscreen"]) {
    [self.window toggleFullScreen:nil];
  } else if ([action isEqualToString:@"close"]) {
    [self.window performClose:nil];
  } else if ([action isEqualToString:@"quit"]) {
    [NSApp terminate:nil];
  }
}

- (WKWebView*)webView:(WKWebView*)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration*)configuration
               forNavigationAction:(WKNavigationAction*)navigationAction
                    windowFeatures:(WKWindowFeatures*)windowFeatures {
  (void)webView;
  (void)configuration;
  (void)windowFeatures;
  NSURL* url = navigationAction.request.URL;
  if (url == nil || ![[url host] isEqualToString:@"127.0.0.1"]) return nil;
  spawn_self([[url absoluteString] UTF8String]);
  return nil;
}

- (void)applicationWillTerminate:(NSNotification*)notification {
  (void)notification;
  stop_core();
}

@end

namespace {

std::string instance_socket_path() {
  const char* tmp = std::getenv("TMPDIR");
  std::string dir = tmp != nullptr && tmp[0] != '\0' ? tmp : "/tmp";
  while (!dir.empty() && dir.back() == '/') dir.pop_back();
  return dir + "/molink-shell-" + std::to_string(::getuid()) + ".sock";
}

int connect_instance(const std::string& path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
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

/// Returns true when this process owns the instance socket. Sets
/// @p already_running when another shell accepted the "show" request.
bool claim_instance_socket(const std::string& path, bool& already_running) {
  already_running = false;
  for (int attempt = 0; attempt < 2; ++attempt) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
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
      std::thread([fd] {
        for (;;) {
          const int client = ::accept(fd, nullptr, nullptr);
          if (client < 0) break;
          char buffer[32];
          const ssize_t got = ::read(client, buffer, sizeof(buffer));
          (void)got;
          ::close(client);
          dispatch_async(dispatch_get_main_queue(), ^{
            if (g_app_delegate != nil) [g_app_delegate showWindow];
          });
        }
      }).detach();
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
    // A previous shell may have been killed without unlinking the socket.
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

}  // namespace

int main(int argc, char** argv) {
  @autoreleasepool {
    for (int i = 1; i < argc; ++i) {
      if (std::strcmp(argv[i], "--smoke-test") == 0) {
        g_smoke = true;
      } else if (std::strcmp(argv[i], "--url") == 0 && i + 1 < argc) {
        g_url_override = argv[++i];
      }
    }
    g_url_mode = !g_url_override.empty();

    if (g_url_mode) {
      const std::string url = g_url_override;
      [NSApplication sharedApplication];
      [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
      NSMenu* menubar = [[NSMenu alloc] init];
      NSMenuItem* app_item = [[NSMenuItem alloc] init];
      [menubar addItem:app_item];
      [NSApp setMainMenu:menubar];
      NSMenu* app_menu = [[NSMenu alloc] init];
      [app_menu addItemWithTitle:@"Quit moLink"
                          action:@selector(terminate:)
                   keyEquivalent:@"q"];
      [app_item setSubmenu:app_menu];
      AppDelegate* delegate = [[AppDelegate alloc] init];
      delegate.url = [NSString stringWithUTF8String:url.c_str()];
      [NSApp setDelegate:delegate];
      [NSApp run];
      return 0;
    }

    if (!g_smoke) {
      const std::string instance_path = instance_socket_path();
      bool already_running = false;
      if (!claim_instance_socket(instance_path, already_running)) {
        if (already_running) return 0;
        std::fprintf(stderr,
                     "moLink shell: could not create the single-instance "
                     "socket at %s; starting anyway\n",
                     instance_path.c_str());
      } else {
        std::atexit(cleanup_instance_socket);
      }
    }

    const std::string core = find_core();
    if (core.empty()) {
      std::fprintf(stderr,
                   "moLink shell: molink-core not found; set MOLINK_CORE\n");
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
    std::string error;
    if (!start_core(core, web_root, port, token, client_token, error)) {
      std::fprintf(stderr, "moLink shell: %s\n", error.c_str());
      stop_core();
      return 1;
    }

    std::string url = "http://127.0.0.1:" + std::to_string(port) +
                      "/?host=127.0.0.1&port=" + std::to_string(port) +
                      "&shell=1&platform=macos";
    if (!client_token.empty()) url += "&client=" + client_token;

    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    NSMenu* menubar = [[NSMenu alloc] init];
    NSMenuItem* app_item = [[NSMenuItem alloc] init];
    [menubar addItem:app_item];
    [NSApp setMainMenu:menubar];

    NSMenu* app_menu = [[NSMenu alloc] init];
    NSMenuItem* fullscreen = [[NSMenuItem alloc]
        initWithTitle:@"Toggle Full Screen"
               action:@selector(toggleFullScreen:)
        keyEquivalent:@"f"];
    fullscreen.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagControl;
    [app_menu addItem:fullscreen];
    [app_menu addItem:[NSMenuItem separatorItem]];
    [app_menu addItemWithTitle:@"Quit moLink"
                        action:@selector(terminate:)
                 keyEquivalent:@"q"];
    [app_item setSubmenu:app_menu];

    AppDelegate* delegate = [[AppDelegate alloc] init];
    delegate.url = [NSString stringWithUTF8String:url.c_str()];
    g_app_delegate = delegate;
    [NSApp setDelegate:delegate];
    [NSApp run];
  }
  return 0;
}
