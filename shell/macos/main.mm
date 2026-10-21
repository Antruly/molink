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
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

extern char** environ;

namespace {

namespace fs = std::filesystem;

pid_t g_core_pid = -1;
bool g_smoke = false;
bool g_smoke_done = false;

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
                std::string& token, std::string& error) {
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
      !json_string(line, "token", token) || parsed_port <= 0) {
    error = "molink-core did not report a ready port and token";
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

@interface AppDelegate : NSObject <NSApplicationDelegate, WKNavigationDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) WKWebView* webView;
@property(nonatomic, copy) NSString* url;
@end

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
  self.window.backgroundColor =
      [NSColor colorWithCalibratedRed:5.0 / 255.0
                                green:13.0 / 255.0
                                 blue:11.0 / 255.0
                                alpha:1.0];

  WKWebViewConfiguration* configuration =
      [[WKWebViewConfiguration alloc] init];
  self.webView = [[WKWebView alloc] initWithFrame:frame
                                    configuration:configuration];
  self.webView.navigationDelegate = self;
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
  return YES;
}

- (void)applicationWillTerminate:(NSNotification*)notification {
  (void)notification;
  stop_core();
}

@end

int main(int argc, char** argv) {
  @autoreleasepool {
    for (int i = 1; i < argc; ++i) {
      if (std::strcmp(argv[i], "--smoke-test") == 0) g_smoke = true;
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
    std::string error;
    if (!start_core(core, web_root, port, token, error)) {
      std::fprintf(stderr, "moLink shell: %s\n", error.c_str());
      stop_core();
      return 1;
    }

    const std::string url = "http://127.0.0.1:" + std::to_string(port) +
                            "/?host=127.0.0.1&port=" + std::to_string(port) +
                            "&token=" + token;

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
    [NSApp setDelegate:delegate];
    [NSApp run];
  }
  return 0;
}
