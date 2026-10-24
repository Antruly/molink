#pragma once

#include <string>

#include <db/uvcpp_db.h>

#include "db/database.h"

namespace molink {

/// Import a 0.1.x `devices.json` when the devices table is empty. The file is
/// renamed to `devices.json.imported.<epoch>` only after a successful commit.
bool import_legacy_devices(database& db, uvcpp::uvcpp_db_client& client,
                           const std::string& data_dir, std::string& error);

}  // namespace molink
