#include <db/uvcpp_db.h>

#include <cstdio>
#include <filesystem>

#include "config.h"
#include "db/database.h"
#include "version.h"

int main() {
  using namespace molink;
  std::error_code ec;
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path(ec) /
      "molink-server-foundation-test";
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    std::fprintf(stderr, "temp dir: %s\n", ec.message().c_str());
    return 1;
  }

  database_config config;
  config.driver = "sqlite";
  config.url = sqlite_url_for(dir.string());
  config.pool_min = 1;
  config.pool_max = 2;
  config.busy_timeout_ms = 2000;

  database db(config);
  std::string err;
  if (!db.open(err)) {
    std::fprintf(stderr, "open: %s\n", err.c_str());
    return 1;
  }
  if (!db.migrate(err)) {
    std::fprintf(stderr, "migrate: %s\n", err.c_str());
    return 1;
  }
  if (db.schema_version() < 3) {
    std::fprintf(stderr, "unexpected schema version %d\n", db.schema_version());
    return 1;
  }

  uvcpp::uvcpp_db_client* client = db.acquire();
  if (client == nullptr) {
    std::fprintf(stderr, "acquire: %s\n", db.last_error().c_str());
    return 1;
  }
  auto status = client->execute(
      "INSERT INTO devices (id, token_hash, name, port, created_ms, "
      "last_seen_ms) VALUES (?, ?, ?, ?, ?, ?)",
      uvcpp::uvcpp_db_params{std::string("TESTID123"), std::string("hash"),
                             std::string("smoke"), 28418,
                             static_cast<int64_t>(1),
                             static_cast<int64_t>(1)});
  if (!uvcpp::uvcpp_db_ok(status)) {
    std::fprintf(stderr, "insert: %s\n", client->last_error().c_str());
    db.release(client);
    return 1;
  }
  uvcpp::uvcpp_db_table table;
  status = client->query(
      "SELECT name FROM devices WHERE id = ?",
      uvcpp::uvcpp_db_params{std::string("TESTID123")}, &table);
  if (!uvcpp::uvcpp_db_ok(status) || table.row_count() != 1 ||
      table.value("name").to_text() != "smoke") {
    std::fprintf(stderr, "query: %s\n", client->last_error().c_str());
    db.release(client);
    return 1;
  }
  db.release(client);

  std::printf("ok: %s schema=%d driver=%s\n", MOLINK_SERVER_VERSION,
              db.schema_version(), db_driver_name(db.dialect().driver()));
  db.close();
  std::filesystem::remove_all(dir, ec);
  return 0;
}
