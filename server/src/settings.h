#pragma once

#include <string>

namespace molink {

/// Default values for the settings rows seeded on first start. They are stored
/// as JSON text so the admin UI can evolve them without a schema migration.
std::string default_server_settings_json();
std::string default_security_settings_json();
std::string default_relay_settings_json();

}  // namespace molink
