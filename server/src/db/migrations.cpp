#include "db/migrations.h"

namespace molink {

namespace {

const char* kDeviceTable =
    "CREATE TABLE IF NOT EXISTS devices ("
    "id TEXT PRIMARY KEY,"
    "token_hash TEXT NOT NULL,"
    "name TEXT NOT NULL,"
    "address TEXT,"
    "peer_ip TEXT,"
    "port INTEGER,"
    "version TEXT,"
    "search_key TEXT,"
    "created_ms INTEGER NOT NULL,"
    "last_seen_ms INTEGER NOT NULL)";

const char* kAdminUserTable =
    "CREATE TABLE IF NOT EXISTS admin_users ("
    "id TEXT PRIMARY KEY,"
    "username TEXT NOT NULL,"
    "username_norm TEXT NOT NULL UNIQUE,"
    "display_name TEXT,"
    "role TEXT NOT NULL,"
    "password_algo TEXT NOT NULL,"
    "password_iterations INTEGER NOT NULL,"
    "password_salt TEXT NOT NULL,"
    "password_hash TEXT NOT NULL,"
    "created_ms INTEGER NOT NULL,"
    "updated_ms INTEGER NOT NULL,"
    "last_login_ms INTEGER NOT NULL DEFAULT 0,"
    "disabled INTEGER NOT NULL DEFAULT 0,"
    "must_change_password INTEGER NOT NULL DEFAULT 0)";

const char* kSessionTable =
    "CREATE TABLE IF NOT EXISTS sessions ("
    "id TEXT PRIMARY KEY,"
    "token_sha256 TEXT NOT NULL UNIQUE,"
    "user_id TEXT NOT NULL,"
    "created_ms INTEGER NOT NULL,"
    "last_seen_ms INTEGER NOT NULL,"
    "expires_ms INTEGER NOT NULL,"
    "ip TEXT,"
    "user_agent TEXT)";

const char* kBanTable =
    "CREATE TABLE IF NOT EXISTS bans ("
    "id TEXT PRIMARY KEY,"
    "target TEXT NOT NULL,"
    "target_kind TEXT NOT NULL,"
    "reason TEXT,"
    "source TEXT NOT NULL,"
    "created_ms INTEGER NOT NULL,"
    "expires_ms INTEGER NOT NULL,"
    "created_by TEXT,"
    "note TEXT)";

const char* kSettingsTable =
    "CREATE TABLE IF NOT EXISTS settings ("
    "key TEXT PRIMARY KEY,"
    "value_json TEXT NOT NULL,"
    "updated_ms INTEGER NOT NULL)";

const char* kServerStateTable =
    "CREATE TABLE IF NOT EXISTS server_state ("
    "id INTEGER PRIMARY KEY CHECK (id = 1),"
    "setup_completed INTEGER NOT NULL DEFAULT 0,"
    "settings_revision INTEGER NOT NULL DEFAULT 1)";

const char* kAuditColumns =
    "ts_ms INTEGER NOT NULL,"
    "event TEXT NOT NULL,"
    "result TEXT NOT NULL,"
    "actor_id TEXT,"
    "actor_name TEXT,"
    "role TEXT,"
    "ip TEXT,"
    "target TEXT,"
    "details_json TEXT";

const char* kAuditIndexes[] = {
    "CREATE INDEX IF NOT EXISTS idx_audit_ts ON audit_events(ts_ms)",
    "CREATE INDEX IF NOT EXISTS idx_audit_event ON audit_events(event)",
    "CREATE INDEX IF NOT EXISTS idx_audit_actor ON audit_events(actor_id)",
};

const char* kCommonIndexes[] = {
    "CREATE INDEX IF NOT EXISTS idx_devices_last_seen ON devices(last_seen_ms)",
    "CREATE INDEX IF NOT EXISTS idx_devices_peer ON devices(peer_ip)",
    "CREATE INDEX IF NOT EXISTS idx_devices_search ON devices(search_key)",
    "CREATE INDEX IF NOT EXISTS idx_sessions_user ON sessions(user_id)",
    "CREATE INDEX IF NOT EXISTS idx_sessions_expires ON sessions(expires_ms)",
    "CREATE INDEX IF NOT EXISTS idx_bans_expires ON bans(expires_ms)",
    "CREATE INDEX IF NOT EXISTS idx_bans_target ON bans(target)",
};

std::vector<std::string> device_tables() {
  return {kServerStateTable, kSettingsTable, kDeviceTable, kAdminUserTable,
          kSessionTable, kBanTable};
}

std::vector<std::string> audit_table_sqlite() {
  return {std::string("CREATE TABLE IF NOT EXISTS audit_events (id INTEGER "
                      "PRIMARY KEY AUTOINCREMENT,") +
          kAuditColumns + ")"};
}

std::vector<std::string> audit_table_postgres() {
  return {std::string("CREATE TABLE IF NOT EXISTS audit_events (id BIGSERIAL "
                      "PRIMARY KEY,") +
          kAuditColumns + ")"};
}

std::vector<std::string> indexes(bool postgres) {
  std::vector<std::string> out;
  for (const char* sql : kCommonIndexes) out.emplace_back(sql);
  if (postgres) {
    for (const char* sql : kAuditIndexes) out.emplace_back(sql);
  } else {
    for (const char* sql : kAuditIndexes) out.emplace_back(sql);
  }
  return out;
}

}  // namespace

const std::vector<migration>& schema_migrations() {
  static const std::vector<migration> kMigrations = {
      {1,
       "core tables",
       device_tables(),
       device_tables()},
      {2,
       "audit table",
       audit_table_sqlite(),
       audit_table_postgres()},
      {3,
       "indexes",
       indexes(false),
       indexes(true)},
  };
  return kMigrations;
}

}  // namespace molink
