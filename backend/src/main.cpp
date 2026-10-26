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

#include <net/uvcpp_tcp_client.h>
#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_json.h>

#include "api.h"
#include "audio/audio.h"
#include "audio/voice.h"
#include "auth_state.h"
#include "call_hub.h"
#include "capture/capture.h"
#include "chat_hub.h"
#include "clipboard/clipboard.h"
#include "config.h"
#include "discovery.h"
#include "identity.h"
#include "tls_identity.h"
#include "lock_state.h"
#include "network_settings.h"
#include "input/input.h"
#if defined(_WIN32)
#include "input/windows_secure_input.h"
#endif
#include "server_client.h"
#include "p2p/p2p_manager.h"
#include "transport/connect_proxy.h"
#include "ui_store.h"
#include "session_registry.h"
#include "util.h"
#include "version.h"
#include "service_control.h"
#include "session_controller.h"
#include "transport/ws_transport.h"

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
      << "  --host <addr>        bind address (default 0.0.0.0; use 127.0.0.1/::1\n"
      << "                       for local-only access)\n"
      << "  --port <n>           web/API/WS port (default 28418; 0 = ephemeral)\n"
      << "  --http3-port <n>     HTTP/3 QUIC web port (default 28419; reserved)\n"
      << "  --quic-port <n>      direct QUIC data port (default 28420; reserved)\n"
      << "  --tcp-data-port <n>  direct TCP data port (default 28421; reserved)\n"
      << "  --udp-port <n>       direct UDP data port (default 28422; reserved)\n"
      << "  --token <secret>     shared token; generated if omitted\n"
      << "  --allow-remote       permit an explicitly chosen non-loopback\n"
      << "                       --host\n"
      << "  --parent-pid <pid>   exit when that process disappears\n"
      << "  --capture-fps <n>    screen capture frames per second (default 15)\n"
      << "  --tile-size <n>      tile edge in pixels (0 = auto; default 0)\n"
      << "  --change-threshold <n>  per-channel delta treated as unchanged\n"
      << "                        (default 4; 0 = exact comparison)\n"
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
                 const std::string& token, const std::string& client_token,
                 const std::string& capture_backend,
                 const std::string& input_backend,
                 const std::string& device_id, bool http2_available,
                 int tile_size, int change_threshold, bool tls_enabled,
                 const std::string& cert_fingerprint) {
  uvcpp::uvcpp_json j;
  j["event"] = "ready";
  j["service"] = MOLINK_CORE_SERVICE;
  j["version"] = MOLINK_VERSION;
  j["protocol"] = 1;
  j["pid"] = static_cast<long long>(MOLINK_GETPID());
  j["host"] = cfg.host;
  j["port"] = port;
  j["token"] = token;
  j["client_token"] = client_token;
  j["session_endpoint"] = "/ws/session";
  j["transport"] = "websocket";
  j["tls"] = tls_enabled;
  if (!cert_fingerprint.empty()) j["tls_fingerprint"] = cert_fingerprint;
  j["session_scheme"] = tls_enabled ? "wss" : "ws";
  j["transports"] = uvcpp::uvcpp_json::array();
  j["transports"].push_back("websocket");
  if (http2_available) j["transports"].push_back("http2");
  j["ports"]["web"] = port;
  j["ports"]["http3"] = cfg.http3_port;
  j["ports"]["quic"] = cfg.quic_port;
  j["ports"]["tcp_data"] = cfg.tcp_data_port;
  j["ports"]["udp_data"] = cfg.udp_port;
  j["device_id"] = device_id;
  j["capture"] = capture_backend;
  j["tile_size"] = tile_size;
  j["change_threshold"] = change_threshold;
  j["input"] = input_backend;
  j["synthetic_frames"] = capture_backend == "synthetic";
  std::cout << uvcpp::uvcpp_json_dump(j) << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace molink;

#if defined(_WIN32)
  // Keep GetSystemMetrics/BitBlt/SendInput in physical pixels. System DPI
  // awareness (SetProcessDPIAware) is virtualized on a per-monitor scaling
  // different from the system DPI: a 1920x1080 screen at 112.5% then reports
  // and captures 1707x960 logical pixels. Prefer per-monitor v2, fall back to
  // per-monitor, then to the Windows 7 system-DPI API.
  using set_dpi_ctx_fn = BOOL(WINAPI*)(void*);
  using set_dpi_awareness_fn = HRESULT(WINAPI*)(int);
  bool dpi_ok = false;
  if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
    auto set_ctx = reinterpret_cast<set_dpi_ctx_fn>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (set_ctx != nullptr) {
      // PER_MONITOR_AWARE_V2 = -4
      dpi_ok = set_ctx(reinterpret_cast<void*>(-4)) != FALSE;
    }
  }
  if (!dpi_ok) {
    // shcore.dll is not loaded into every process; load it explicitly so the
    // per-monitor fallback also works on Windows 8.1.
    if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
      auto set_awareness = reinterpret_cast<set_dpi_awareness_fn>(
          GetProcAddress(shcore, "SetProcessDpiAwareness"));
      if (set_awareness != nullptr) {
        // PROCESS_PER_MONITOR_DPI_AWARE = 2
        dpi_ok = SUCCEEDED(set_awareness(2));
      }
    }
  }
  if (!dpi_ok) SetProcessDPIAware();
#endif

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

  std::string identity_error;
  const device_identity identity = load_identity(cfg, identity_error);
  cfg.token = identity.token;
  if (!identity_error.empty()) {
    std::fprintf(stderr, "molink identity: %s\n", identity_error.c_str());
  }

  // Security settings own the long-term code and the bind policy. Existing
  // installations migrate the old token file into the long-term slot.
  app_settings stored_settings = load_settings();
  if (stored_settings.long_term_code.empty()) {
    stored_settings.long_term_code = identity.token;
  }
  auto auth = std::make_shared<auth_state>(std::move(stored_settings));
  // Self-signed per-device certificate. It is generated once and reused so a
  // client's fingerprint pin survives restarts. TLS is opt-in (LAN default
  // off); the relay path keeps using the directory's WSS.
  const tls_identity tls = load_or_create_tls_identity(config_directory(),
                                                       identity.device_id);
  if (!tls.ok()) {
    std::fprintf(stderr, "molink tls: %s\n", tls.error.c_str());
  }
  const bool tls_enabled = auth->settings().encrypt_lan && tls.ok();
  // Only the shell receives this token (through the ready line). It gates the
  // loopback web UI when allow_web_client is off.
  const std::string client_token = generate_token();
  // The listener stays on 0.0.0.0 so the Security toggle can take effect
  // immediately; `allow_remote_access` is enforced by the connection hook
  // below. An explicit --host 127.0.0.1 still binds local-only.

  auto registry = std::make_shared<session_registry>();
  auto access_requests = std::make_shared<access_request_registry>();
  auto chat = std::make_shared<chat_hub>();
  std::string audio_error;
  std::unique_ptr<audio_source> audio_source = make_audio_source(audio_error);
  std::shared_ptr<audio_hub> audio;
  if (audio_source) {
    audio = std::make_shared<audio_hub>(std::move(audio_source));
    const audio_format format = audio->format();
    std::fprintf(stderr, "molink: audio loopback %d Hz, %d channel(s)\n",
                 format.sample_rate, format.channels);
  }
  auto call = std::make_shared<call_hub>();
  std::string voice_error;
  std::shared_ptr<voice_engine> voice(make_voice_engine(voice_error));
  if (voice) call->set_host_microphone(voice->microphone_available());
  const uint64_t started_ms = now_ms();

  capture_options capture_opts;
  capture_opts.fps = cfg.capture_fps;
  capture_opts.tile_size =
      cfg.tile_size > 0 ? static_cast<uint16_t>(cfg.tile_size)
                        : choose_tile_size();
  capture_opts.change_threshold = cfg.change_threshold;
  capture_opts.compress = !cfg.no_compress;
  // Start the optional session service first: it launches the secure-desktop
  // helper, which start_capture() may prefer over DXGI.
  std::string service_error;
  if (auth->settings().service_desired_running) {
    start_session_service(service_error);
  } else {
    stop_session_service(service_error);
  }
  std::string capture_note;
  std::shared_ptr<capture_source> capture =
      start_capture(capture_opts, !cfg.no_capture, capture_note);
  if (!capture) {
    std::cerr << "molink-core: " << capture_note << "\n";
    return 1;
  }

  if (!cfg.port_explicit) {
    const int saved = load_port_setting("tcp_port", -1);
    if (saved > 0) cfg.port = saved;
  }
  if (!cfg.http3_port_explicit) {
    const int saved = load_port_setting("http3_port", -1);
    if (saved > 0) cfg.http3_port = saved;
  }
  if (!cfg.quic_port_explicit) {
    const int saved = load_port_setting("quic_port", -1);
    if (saved > 0) cfg.quic_port = saved;
  }
  if (!cfg.tcp_data_port_explicit) {
    const int saved = load_port_setting("tcp_data_port", -1);
    if (saved > 0) cfg.tcp_data_port = saved;
  }
  if (!cfg.udp_port_explicit) {
    const int saved = load_port_setting("udp_port", -1);
    if (saved > 0) cfg.udp_port = saved;
  }
  auto network = std::make_shared<network_settings>();
  network->tcp_port.store(cfg.port);
  network->http3_port.store(cfg.http3_port);
  network->quic_port.store(cfg.quic_port);
  network->tcp_data_port.store(cfg.tcp_data_port);
  network->udp_port.store(cfg.udp_port);

  int lock_timeout = cfg.lock_timeout_seconds;
  if (!cfg.lock_timeout_explicit) {
    const int saved = load_lock_timeout_setting(-1);
    if (saved >= 0) lock_timeout = saved;
  }
  auto lock = std::make_shared<lock_state>(lock_timeout);

  session_options session_opts;
  session_opts.auth = auth;
  session_opts.access_requests = access_requests;
  session_opts.version = MOLINK_VERSION;
  session_opts.capture = capture;

  std::shared_ptr<input_injector> input;
  if (!cfg.no_input) {
    std::string input_error;
#if defined(_WIN32)
    std::unique_ptr<input_injector> created;
    if (capture->backend() == "secure-desktop") {
      created = make_windows_secure_input();
    }
    if (!created) created = make_windows_input(input_error);
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
  session_opts.input = input;
  if (input) {
    const display_info display = capture->current_display_info();
    if (display.input_width > 0 && display.input_height > 0) {
      input->set_display_rect(display.x, display.y, display.input_width,
                              display.input_height);
    }
  }

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
  session_opts.clipboard = clipboard;
  session_opts.device_id = identity.device_id;
  session_opts.device_name = auth->settings().device_name;
  session_opts.cert_fingerprint = tls.fingerprint;
  session_opts.tls_enabled = tls_enabled;
  session_opts.lock = lock;

  uvcpp::uvcpp_web_app app;
  session_opts.post = [&app](std::function<void()> fn) { app.post(std::move(fn)); };
  app.set_host(cfg.host)
      .set_port(cfg.port)
      .set_server_header(std::string(MOLINK_PRODUCT_NAME) + "/" + MOLINK_VERSION)
      .set_access_log(false)
      .set_max_body_size(1024u * 1024u);

  // Hot-swappable LAN access: the listener stays on 0.0.0.0, and when the
  // setting is off a non-loopback TCP connection is closed before HTTP parsing.
  // This is what lets the Security switch apply without restarting the app.
  app.on_connection([auth](uvcpp::uvcpp_web_conn_id,
                           uvcpp::uvcpp_tcp_client* client) {
    if (client == nullptr) return;
    std::string ip;
    int port = 0;
    client->getPeerAddrs(ip, port);
    if (is_loopback_peer(ip)) return;
    if (auth->settings().allow_remote_access) return;
    client->close();
  });

  // h2c is available in libuvcpp but its request dispatch hangs against the
  // curl h2c client in the 1.8.x builds we tested, so it stays off until that
  // is verified. `/api/transports` reports http2 as unavailable in this
  // build; the client negotiation ladder already knows where it will sit.

  const std::string web_root = resolve_web_root(cfg);
  auto bound_port = std::make_shared<std::atomic<int>>(cfg.port);
  auto server_client_state =
      std::make_shared<server_client>(auth, bound_port, MOLINK_VERSION);
  auto discovery = std::make_shared<discovery_service>(
      auth, identity.device_id, cfg.port);
  auto ui_preferences = std::make_shared<ui_store>();
  std::string ui_error;
  if (!ui_preferences->load(ui_error)) {
    std::fprintf(stderr, "molink ui preferences: %s\n", ui_error.c_str());
  }

  api_deps deps;
  deps.bound_port = bound_port;
  deps.ui = ui_preferences;
  deps.discovery = discovery;
  deps.serve_web = !web_root.empty();
  deps.registry = registry;
  deps.capture = capture;
  deps.input = input;
  deps.clipboard = clipboard;
  deps.device_id = identity.device_id;
  deps.cert_fingerprint = tls.fingerprint;
  deps.client_token = client_token;
  deps.auth = auth;
  deps.access_requests = access_requests;
  deps.chat = chat;
  deps.call = call;
  deps.server = server_client_state;
  deps.lock = lock;
  deps.network = network;
  deps.config = cfg;
  deps.started_ms = started_ms;
  deps.http2_available = app.h2c_enabled();
  deps.tile_size = capture_opts.tile_size;
  deps.change_threshold = capture_opts.change_threshold;
  session_opts.network = network;
  session_opts.chat = chat;
  session_opts.audio = audio;
  session_opts.call = call;
  session_opts.voice = voice;
  session_opts.bound_port = bound_port;
  session_opts.http2_available = deps.http2_available;
  session_opts.tile_size = capture_opts.tile_size;
  auto p2p = std::make_shared<p2p_manager>(
      app.loop(),
      [&app](std::function<void()> fn) { app.post(std::move(fn)); },
      server_client_state, registry, session_opts);
  register_http_routes(app, deps);
  app.websocket("/ws/session", make_ws_handler(registry, session_opts));
  app.websocket("/ws/connect", make_connect_handler(
      p2p, [&app](std::function<void()> fn) { app.post(std::move(fn)); }));

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

#if !UVCPP_OPENSSL_ENABLE
  // The core may have its own OpenSSL (it generates the device certificate),
  // but HTTPS is served by libuvcpp. A libuvcpp built without TLS has no
  // enable_ssl at all, so the settings must report encryption as unavailable
  // instead of failing to compile or silently serving plain HTTP.
  if (tls_enabled) {
    std::fprintf(stderr,
                 "molink tls: libuvcpp was built without TLS; encryption is "
                 "unavailable\n");
    tls_enabled = false;
  }
#endif
#if UVCPP_OPENSSL_ENABLE
  if (tls_enabled) {
    app.enable_ssl(tls.cert_path, tls.key_path);
    if (!app.ssl_error().empty()) {
      std::fprintf(stderr, "molink tls: %s\n", app.ssl_error().c_str());
    }
    // h2c is disabled in this build; advertising h2 over TLS would make a
    // browser negotiate a protocol the router does not serve. Offer only
    // HTTP/1.1 (WSS rides on it) unless h2 becomes available.
    if (app.ssl_context() != nullptr) {
      if (app.h2c_enabled()) {
        app.ssl_context()->set_alpn_select_protos({"h2", "http/1.1"});
      } else {
        app.ssl_context()->set_alpn_select_protos({"http/1.1"});
      }
    }
  }
#endif

  // libuvcpp's start() returns once the loop is running; the stop below must
  // wait for the shutdown path, or it would stop the service we just started.
  const int rc = app.start();
  if (rc != 0) {
    // Stop capture before returning: its thread owns a joinable std::thread,
    // and unwinding past it would call std::terminate.
    capture->stop();
    std::cerr << "molink-core: failed to listen on "
              << format_url_host(cfg.host) << ":"
              << cfg.port << " (libuv error " << rc << ")\n";
    return 1;
  }

  const int port = app.bound_port();
  bound_port->store(port);
  server_client_state->start();
  p2p->start();
  discovery->set_web_port(port);
  {
    const app_settings current = auth->settings();
    discovery->set_enabled(current.allow_remote_access &&
                           current.discovery_enabled);
  }

  session_supervisor supervisor(app, registry, session_opts);
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

  const code_view display_code = auth->view();
  if (cfg.print_ready_json) {
    print_ready(cfg, port, auth->local_session_token(), client_token,
                capture->backend(), input ? input->backend() : "none",
                identity.device_id, deps.http2_available,
                capture_opts.tile_size, capture_opts.change_threshold,
                tls_enabled, tls.fingerprint);
  } else {
    std::cerr << MOLINK_CORE_SERVICE << " " MOLINK_VERSION << " listening on "
              << format_url_host(cfg.host) << ":" << port << "\n"
              << "  session: " << (tls_enabled ? "wss://" : "ws://")
              << format_url_host(cfg.host) << ":" << port
              << "/ws/session\n"
              << "  health:  http://" << format_url_host(cfg.host) << ":"
              << port
              << "/healthz\n"
              << "  capture: " << capture_note << "\n"
              << "  tiles:   " << capture_opts.tile_size << " px"
              << " (change threshold " << cfg.change_threshold << ")\n"
              << "  device:  " << identity.device_id << "\n"
              << "  lock:    " << lock_timeout << " s idle\n"
              << "  udp:     " << cfg.udp_port << " (reserved)\n"
              << "  input:   " << (input ? input->backend() : "disabled")
              << "\n"
              << "  web:     "
              << (web_root.empty() ? "(none)" : web_root) << "\n"
              << "  code:    " << display_code.value << " ("
              << display_code.mode << ", remaining " << display_code.remaining
              << ")\n"
              << "  security: long-term code in Preferences > Security; "
              << "remote access "
              << (auth->settings().allow_remote_access ? "on" : "off")
              << "\n";
  }

  while (g_stop == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  if (parent_watch.joinable()) parent_watch.join();
  supervisor.stop();
  discovery->set_enabled(false);
  capture->stop();
  clipboard.reset();
  // Stop the helper only on a real exit. It is started above when the user
  // asked for it, and the app's own lifetime defines when it is no longer
  // needed.
  stop_session_service(service_error);
  p2p->stop();
  server_client_state->stop();
  app.stop();
  app.join();
  return 0;
}
