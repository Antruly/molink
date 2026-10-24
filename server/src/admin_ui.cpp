#include "admin_ui.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>
#include <system_error>

#include <nlohmann/json.hpp>

#include "http_common.h"
#include "logging.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace molink {
namespace {

std::filesystem::path executable_path() {
#if defined(_WIN32)
  std::wstring buffer(32768, L'\0');
  const DWORD length =
      GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) return {};
  buffer.resize(length);
  return std::filesystem::path(buffer);
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buffer(size, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
  return std::filesystem::path(buffer.c_str());
#else
  std::error_code ec;
  const std::filesystem::path path =
      std::filesystem::read_symlink("/proc/self/exe", ec);
  return ec ? std::filesystem::path() : path;
#endif
}

bool has_index(const std::filesystem::path& dir) {
  std::error_code ec;
  return std::filesystem::exists(dir / "index.html", ec);
}

}  // namespace

std::string resolve_admin_ui_root(const server_config& config) {
  std::vector<std::filesystem::path> candidates;
  if (!config.admin.web_root.empty()) candidates.emplace_back(config.admin.web_root);
  const char* env = std::getenv("MOLINK_ADMIN_WEB_ROOT");
  if (env != nullptr && *env != '\0') candidates.emplace_back(env);

  const std::filesystem::path exe = executable_path();
  if (!exe.empty()) {
    const std::filesystem::path dir = exe.parent_path();
    candidates.push_back(dir / "web-admin");
    candidates.push_back(dir / "../share/molink-server/web-admin");
    candidates.push_back(dir / "../../server/ui/dist");
  }
  candidates.emplace_back("server/ui/dist");
  candidates.emplace_back("/opt/molink-server/share/web-admin");
  candidates.emplace_back("/usr/local/molink-server/share/web-admin");
  candidates.emplace_back("/usr/share/molink-server/web-admin");

  for (const std::filesystem::path& candidate : candidates) {
    if (has_index(candidate)) return candidate.lexically_normal().string();
  }
  return std::string();
}

void register_admin_ui(uvcpp::uvcpp_web_app& app, const std::string& root) {
  if (root.empty()) {
    app.get("/", [](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& response,
                    uvcpp::uvcpp_web_next) {
      nlohmann::json body;
      body["service"] = "moLink Server admin";
      body["message"] =
          "the admin UI is not staged; build server/ui and pass --ui-dir";
      send_json(response, 200, body);
    });
    log_warn("admin UI is not staged; only /api/admin is available");
    return;
  }
  uvcpp::uvcpp_web_static_options assets;
  assets.cache_control = "public, max-age=31536000, immutable";
  app.serve_static("/assets", root + "/assets", assets);

  uvcpp::uvcpp_web_static_options page;
  page.index_files = {"index.html"};
  page.spa_fallback = true;
  page.spa_file = "index.html";
  page.cache_control = "no-cache";
  app.serve_static("/", root, page);
  log_info("admin UI: %s", root.c_str());
}

}  // namespace molink
