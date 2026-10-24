#pragma once

#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

namespace molink {

/// Persists small UI preferences (UI mode, saved devices, recents) in
/// `config_directory()/ui.json`. The core uses a new port on every launch, so
/// localStorage cannot be the only copy.
class ui_store {
 public:
  bool load(std::string& err);
  nlohmann::json get() const;
  bool save(const nlohmann::json& document, std::string& err);

 private:
  mutable std::mutex mutex_;
  std::string path_;
  nlohmann::json document_ = nlohmann::json::object();
};

}  // namespace molink
