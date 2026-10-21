#include <atomic>
#include <chrono>
#include <filesystem>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_json.h>

#include "api.h"
#include "capture/capture.h"
#include "clipboard/clipboard.h"
#include "config.h"
#include "input/input.h"
#include "session_registry.h"
#include "util.h"
#include "version.h"
#include "ws_session.h"

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#define MOLINK_GETPID _getpid
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#define MOLINK_GETPID getpid
#endif

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

bool process_alive(long pid) {
#ifdef _WIN32
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                         static_cast<DWORD>(pid));
  if (h == nullptr) return false;
  DWORD code = 0;
  const bool ok = GetExitCodeProcess(h, &code) != 0 && code == STILL_ACTIVE;
  CloseHandle(h);
  return ok;
#else
  if (pid <= 0) return false;
  // Signal 0 performs the permission and existence checks without delivering
  // anything. EPERM still means the process exists.
  return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

/// Find the built frontend. Explicit configuration wins, then the environment,
/// then the layouts used by the development tree and installed bundles.
std::string resolve_web_root(const molink::core_config& cfg) {
  namespace fs = std::filesystem;
  if (!cfg.web_root.empty()) return cfg.web_root;
  if (const char* env = std::getenv("MOLINK_WEB_ROOT")) return env;

  std::vector<fs::path> candidates;
  candidates.push_back("frontend/dist");
  candidates.push_back("../frontend/dist");
  std::error_code ec;
#if defined(_WIN32)
  wchar_t module_path[32768] = {};
  const DWORD length =
      GetModuleFileNameW(nullptr, module_path,
                         static_cast<DWORD>(std::size(module_path)));
  const fs::path exe =
      length > 0 ? fs::path(module_path) : fs::path();
#else
  const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
#endif
  if (!exe.empty()) {
    const fs::path dir = exe.parent_path();
    candidates.push_back(dir / "web");
    candidates.push_back(dir / "../share/molink/web");
    candidates.push_back(dir / "../../frontend/dist");
  }
  for (const fs::path& candidate : candidates) {
    if (fs::exists(candidate / "index.html", ec)) {
      return candidate.lexically_normal().string();
    }
  }
  return std::string();
}

void print_usage() {
  std::cout
      << "molink-core " MOLINK_VERSION "\n\n"
      << "Usage: molink-core [options]\n\n"
      << "  --host <addr>        bind address (default 127.0.0.1)\n"
      << "  --port <n>           bind port (default 21118)\n"
      << "  --token <secret>     shared token; generated if omitted\n"
      << "  --allow-remote       permit a non-loopback --host\n"
      << "  --parent-pid <pid>   exit when that process disappears\n"
      << "  --capture-fps <n>    screen capture frames per second (default 15)\n"
      << "  --no-capture         use synthetic test frames instead of the screen\n"
      << "  --no-input           disable pointer and keyboard injection\n"
      << "  --no-compress        send raw BGRA tiles instead of zlib-compressed\n"
      << "  --no-clipboard       disable clipboard synchronization\n"
      << "  --web-root <dir>     serve a built frontend at / (default: auto)\n"
      << "  --print-ready        print one JSON line on stdout when ready\n"
      << "  --version            print version and exit\n"
      << "  --help               show this help\n";
}

void print_ready(const molink::core_config& cfg, int port,
                 const std::string& token, const std::string& capture_backend,
                 const std::string& input_backend) {
  uvcpp::uvcpp_json j;
  j["event"] = "ready";
  j["service"] = MOLINK_CORE_SERVICE;
  j["version"] = MOLINK_VERSION;
  j["protocol"] = 1;
  j["pid"] = static_cast<long long>(MOLINK_GETPID());
  j["host"] = cfg.host;
  j["port"] = port;
  j["token"] = token;
  j["session_endpoint"] = "/ws/session";
  j["capture"] = capture_backend;
  j["input"] = input_backend;
  j["synthetic_frames"] = capture_backend == "synthetic";
  std::cout << uvcpp::uvcpp_json_dump(j) << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace molink;

  core_config cfg;
  std::string err;
  if (!parse_args(argc, argv, cfg, err)) {
    std::cerr << "molink-core: " << err << "\n";
    return 2;
  }
  if (cfg.show_help) {
    print_usage();
    return 0;
  }
  if (cfg.show_version) {
    std::cout << "molink-core " MOLINK_VERSION " (protocol 1)\n";
    return 0;
  }

  if (cfg.token.empty()) cfg.token = generate_token();

  auto registry = std::make_shared<session_registry>();
  const uint64_t started_ms = now_ms();

  capture_options capture_opts;
  capture_opts.fps = cfg.capture_fps;
  capture_opts.compress = !cfg.no_compress;
  std::string capture_note;
  std::shared_ptr<capture_source> capture =
      start_capture(capture_opts, !cfg.no_capture, capture_note);
  if (!capture) {
    std::cerr << "molink-core: " << capture_note << "\n";
    return 1;
  }

  ws_options wsopt;
  wsopt.token = cfg.token;
  wsopt.version = MOLINK_VERSION;
  wsopt.capture = capture;

  std::shared_ptr<input_injector> input;
  if (!cfg.no_input) {
    std::string input_error;
#if defined(_WIN32)
    std::unique_ptr<input_injector> created = make_windows_input(input_error);
#elif defined(__APPLE__)
    std::unique_ptr<input_injector> created = make_macos_input(input_error);
#else
    std::unique_ptr<input_injector> created = make_x11_input(input_error);
#endif
    if (created) {
      input = std::move(created);
    } else {
      std::fprintf(stderr, "molink input: %s\n", input_error.c_str());
    }
  }
  wsopt.input = input;

  std::shared_ptr<clipboard_service> clipboard;
  if (!cfg.no_clipboard) {
    std::string clipboard_error;
#if defined(_WIN32)
    std::unique_ptr<clipboard_service> created =
        make_windows_clipboard(clipboard_error);
#elif defined(__APPLE__)
    std::unique_ptr<clipboard_service> created =
        make_macos_clipboard(clipboard_error);
#else
    std::unique_ptr<clipboard_service> created =
        make_x11_clipboard(clipboard_error);
#endif
    if (created) {
      clipboard = std::move(created);
    } else {
      std::fprintf(stderr, "molink clipboard: %s\n", clipboard_error.c_str());
    }
  }
  wsopt.clipboard = clipboard;

  uvcpp::uvcpp_web_app app;
  wsopt.post = [&app](std::function<void()> fn) { app.post(std::move(fn)); };
  app.set_host(cfg.host)
      .set_port(cfg.port)
      .set_server_header(std::string(MOLINK_PRODUCT_NAME) + "/" + MOLINK_VERSION)
      .set_access_log(false)
      .set_max_body_size(1024u * 1024u);

  const std::string web_root = resolve_web_root(cfg);
  api_deps deps;
  deps.serve_web = !web_root.empty();
  deps.registry = registry;
  deps.capture = capture;
  deps.input = input;
  deps.clipboard = clipboard;
  deps.config = cfg;
  deps.started_ms = started_ms;
  register_http_routes(app, deps);
  app.websocket("/ws/session", make_ws_handler(registry, wsopt));

  if (!web_root.empty()) {
    uvcpp::uvcpp_web_static_options static_options;
    static_options.index_files = {"index.html"};
    // Asset names are content-hashed, but index.html still needs a
    // revalidation on every launch.
    static_options.cache_control = "no-cache";
    app.serve_static("/", web_root, static_options);
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  const int rc = app.start();
  if (rc != 0) {
    // Stop capture before returning: its thread owns a joinable std::thread,
    // and unwinding past it would call std::terminate.
    capture->stop();
    std::cerr << "molink-core: failed to listen on " << cfg.host << ":"
              << cfg.port << " (libuv error " << rc << ")\n";
    return 1;
  }

  const int port = app.bound_port();
  cfg.port = port;

  session_supervisor supervisor(app, registry, wsopt);
  supervisor.start();

  std::atomic<bool> parent_gone{false};
  std::thread parent_watch;
  if (cfg.parent_pid > 0) {
    parent_watch = std::thread([pid = cfg.parent_pid, &parent_gone] {
      while (!g_stop && !parent_gone.load()) {
        if (!process_alive(pid)) {
          parent_gone.store(true);
          g_stop = 1;
          return;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
    });
  }

  if (cfg.print_ready_json) {
    print_ready(cfg, port, cfg.token, capture->backend(),
                input ? input->backend() : "none");
  } else {
    std::cerr << MOLINK_CORE_SERVICE << " " MOLINK_VERSION << " listening on "
              << cfg.host << ":" << port << "\n"
              << "  session: ws://" << cfg.host << ":" << port
              << "/ws/session\n"
              << "  health:  http://" << cfg.host << ":" << port
              << "/healthz\n"
              << "  capture: " << capture_note << "\n"
              << "  input:   " << (input ? input->backend() : "disabled")
              << "\n"
              << "  web:     "
              << (web_root.empty() ? "(none)" : web_root) << "\n"
              << "  token:   " << cfg.token << "\n";
  }

  while (g_stop == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  if (parent_watch.joinable()) parent_watch.join();
  supervisor.stop();
  capture->stop();
  clipboard.reset();
  app.stop();
  app.join();
  return 0;
}
