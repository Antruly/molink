#include "ui_store.h"

#include <filesystem>
#include <fstream>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include "identity.h"

namespace molink {
namespace {

constexpr std::size_t kMaxBytes = 256u * 1024u;

}  // namespace

bool ui_store::load(std::string& err) {
  const std::string dir = config_directory();
  if (dir.empty()) {
    err = "cannot resolve the configuration directory";
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  path_ = (std::filesystem::path(dir) / "ui.json").string();
  std::ifstream input(path_, std::ios::binary);
  if (!input) {
    document_ = nlohmann::json::object();
    return true;
  }
  try {
    nlohmann::json parsed;
    input >> parsed;
    if (!parsed.is_object()) {
      err = "ui.json must contain a JSON object";
      return false;
    }
    document_ = std::move(parsed);
  } catch (const std::exception& e) {
    err = std::string("ui.json is not valid JSON: ") + e.what();
    return false;
  }
  return true;
}

nlohmann::json ui_store::get() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return document_;
}

bool ui_store::save(const nlohmann::json& document, std::string& err) {
  if (!document.is_object()) {
    err = "UI preferences must be a JSON object";
    return false;
  }
  const std::string text = document.dump();
  if (text.size() > kMaxBytes) {
    err = "UI preferences are too large";
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (path_.empty()) {
    err = "UI preferences path is not initialised";
    return false;
  }
  const std::filesystem::path target(path_);
  const std::filesystem::path temporary = target.string() + ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) {
      err = "cannot write " + temporary.string();
      return false;
    }
    out << text;
    if (!out) {
      err = "cannot write " + temporary.string();
      return false;
    }
  }
#if !defined(_WIN32)
  ::chmod(temporary.c_str(), 0600);
#endif
  std::error_code ec;
  std::filesystem::rename(temporary, target, ec);
  if (ec) {
    err = "cannot replace " + target.string() + ": " + ec.message();
    return false;
  }
  document_ = document;
  return true;
}

}  // namespace molink
