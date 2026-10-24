#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>

#include "config.h"
#include "runtime.h"
#include "version.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop.store(true); }

#if defined(_WIN32)
BOOL WINAPI on_console_event(DWORD type) {
  switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
      g_stop.store(true);
      return TRUE;
    default:
      return FALSE;
  }
}
#endif

void print_usage() {
  std::printf(
      "molink-server %s\n\n"
      "Usage: molink-server [options]\n\n"
      "  --config <file>            JSON configuration file\n"
      "  --host <addr>              device API bind address (default 0.0.0.0)\n"
      "  --port <n>                 device API port (default 28430)\n"
      "  --tls-cert/--tls-key <pem> device API TLS certificate and key\n"
      "  --admin-host <addr>        admin API bind address (default 127.0.0.1)\n"
      "  --admin-port <n>           admin API port (default 28431)\n"
      "  --admin-tls-cert/--admin-tls-key <pem>\n"
      "                             admin API TLS certificate and key\n"
      "  --ui-dir <dir>             built admin SPA directory\n"
      "  --data-dir <dir>           data directory (default ./molink-server-data)\n"
      "  --db-driver <name>         sqlite (default) or postgres\n"
      "  --db-url <url>             sqlite:///path or postgres://user:pass@host/db\n"
      "  --db-pool-min/--db-pool-max <n>\n"
      "  --db-busy-timeout <ms>\n"
      "  --registration-token <t>   require this invite on registration\n"
      "  --public-url <url>         address advertised to clients\n"
      "  --offline-after <seconds>  presence timeout (default 90)\n"
      "  --allow-insecure-admin     permit a non-loopback admin listener without TLS\n"
      "  --setup-token <token>      one-time token for first-run setup\n"
      "  --skip-legacy-import       do not import an old devices.json\n"
      "  --log-level <level>        debug, info, warn or error\n"
      "  --log-file <path>          write logs to a rotating file\n"
      "  --access-log               log HTTP requests\n"
      "  --print-ready              print one JSON line when both listeners are up\n"
      "  --version                  print the server version and exit\n"
      "  --help                     show this help\n",
      MOLINK_SERVER_VERSION);
}

}  // namespace

int main(int argc, char** argv) {
  using namespace molink;

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
#if !defined(_WIN32)
  std::signal(SIGPIPE, SIG_IGN);
#else
  SetConsoleCtrlHandler(on_console_event, TRUE);
#endif

  server_config config;
  std::string error;
  if (!parse_config(argc, argv, config, error)) {
    std::fprintf(stderr, "molink-server: %s\n", error.c_str());
    return 2;
  }
  if (config.show_help) {
    print_usage();
    return 0;
  }
  if (config.show_version) {
    std::printf("molink-server %s\n", MOLINK_SERVER_VERSION);
    return 0;
  }
  if (config.install_service || config.uninstall_service ||
      config.service_mode) {
    std::fprintf(stderr,
                 "molink-server: service mode is not wired into this build yet\n");
    return 2;
  }

  server_runtime runtime(config);
  if (!runtime.initialize(error) || !runtime.start(error)) {
    std::fprintf(stderr, "molink-server: %s\n", error.c_str());
    runtime.request_stop();
    runtime.shutdown();
    return 1;
  }

  while (!g_stop.load() && !runtime.stopping().load()) {
    runtime.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  runtime.request_stop();
  runtime.shutdown();
  return runtime.exit_code();
}
