#include "db/database.h"

#include <db/uvcpp_db.h>

#include <algorithm>
#include <cstdio>
#include <set>

#include "db/migrations.h"
#include "logging.h"
#include "util.h"

#if !defined(UVCPP_DB_ENABLE) || !UVCPP_DB_ENABLE
#error "molink-server requires libuvcpp with -DUVCPP_ENABLE_DB=ON (SQLite backend)."
#endif

namespace molink {
namespace {

bool ok(uvcpp::uvcpp_db_status status) { return uvcpp::uvcpp_db_ok(status); }

std::string status_text(uvcpp::uvcpp_db_status status,
                        const std::string& detail) {
  std::string text = uvcpp::uvcpp_db_status_name(status);
  if (!detail.empty()) {
    text += ": ";
    text += detail;
  }
  return text;
}

class client_guard {
 public:
  client_guard(database& db, int timeout_ms)
      : db_(db), client_(db.acquire(timeout_ms)) {}
  ~client_guard() {
    if (client_ != nullptr) db_.release(client_);
  }
  client_guard(const client_guard&) = delete;
  client_guard& operator=(const client_guard&) = delete;

  uvcpp::uvcpp_db_client* get() const { return client_; }
  explicit operator bool() const { return client_ != nullptr; }

 private:
  database& db_;
  uvcpp::uvcpp_db_client* client_;
};

}  // namespace

database::database(database_config config)
    : config_(std::move(config)), dialect_(db_driver::sqlite) {}

database::~database() { close(); }

bool database::open(std::string& err) {
  bool driver_ok = false;
  dialect_ = sql_dialect(parse_db_driver(config_.driver, driver_ok));
  if (!driver_ok) {
    err = "unknown database driver: " + config_.driver;
    return false;
  }
  url_ = config_.url;
  if (url_.empty()) {
    err = "database URL is empty";
    return false;
  }

  pool_.set_native_async(false);
  pool_.set_acquire_timeout_ms(std::max(1000, config_.busy_timeout_ms));
  pool_.set_timeout_ms(config_.busy_timeout_ms);

  const uvcpp::uvcpp_db_status status =
      pool_.init(url_, static_cast<std::size_t>(config_.pool_min),
                 static_cast<std::size_t>(config_.pool_max));
  if (!ok(status)) {
    err = status_text(status, pool_.last_error());
    return false;
  }
  if (dialect_.driver() == db_driver::sqlite &&
      !apply_sqlite_settings(err)) {
    pool_.close();
    return false;
  }
  log_info("database: %s (schema pending)", url_.c_str());
  return true;
}

bool database::apply_sqlite_settings(std::string& err) {
  client_guard guard(*this, 5000);
  if (!guard) {
    err = "database pool has no connection";
    return false;
  }
  uvcpp::uvcpp_db_client* client = guard.get();
  // WAL is persistent in the database; the other two are per connection.
  // busy_timeout is configured on the pool before connections are opened.
  uvcpp::uvcpp_db_table table;
  const auto journal = client->query("PRAGMA journal_mode=WAL", &table);
  if (!ok(journal)) {
    err = status_text(journal, client->last_error());
    return false;
  }
  for (const char* pragma : {"PRAGMA synchronous=NORMAL",
                            "PRAGMA foreign_keys=ON"}) {
    const auto status = client->execute(pragma);
    if (!ok(status)) {
      err = status_text(status, client->last_error());
      return false;
    }
  }
  return true;
}

bool database::migrate(std::string& err) {
  client_guard guard(*this, 5000);
  if (!guard) {
    err = "database pool has no connection";
    return false;
  }
  uvcpp::uvcpp_db_client* client = guard.get();

  const char* create_schema_table =
      "CREATE TABLE IF NOT EXISTS schema_migrations ("
      "version INTEGER PRIMARY KEY,"
      "name TEXT NOT NULL,"
      "applied_ms INTEGER NOT NULL)";
  auto status = client->execute(create_schema_table);
  if (!ok(status)) {
    err = status_text(status, client->last_error());
    return false;
  }

  uvcpp::uvcpp_db_table applied_table;
  status = client->query("SELECT version FROM schema_migrations",
                         &applied_table);
  if (!ok(status)) {
    err = status_text(status, client->last_error());
    return false;
  }
  std::set<long long> applied;
  for (const uvcpp::uvcpp_db_row& row : applied_table.rows()) {
    applied.insert(row["version"].to_int64());
  }

  for (const migration& step : schema_migrations()) {
    if (applied.count(step.version) != 0) continue;
    if (!ok(client->begin())) {
      err = "could not begin migration transaction: " + client->last_error();
      return false;
    }
    const std::vector<std::string>& statements =
        dialect_.driver() == db_driver::postgres ? step.postgres_sql
                                                 : step.sqlite_sql;
    bool step_ok = true;
    std::string step_error;
    for (const std::string& sql : statements) {
      const auto step_status = client->execute(sql);
      if (!ok(step_status)) {
        step_ok = false;
        step_error = status_text(step_status, client->last_error());
        break;
      }
    }
    if (step_ok) {
      const std::string insert =
          "INSERT INTO schema_migrations (version, name, applied_ms) VALUES (" +
          dialect_.placeholder(1) + ", " + dialect_.placeholder(2) + ", " +
          dialect_.placeholder(3) + ")";
      const uvcpp::uvcpp_db_params params{
          step.version, step.name, static_cast<int64_t>(epoch_ms())};
      const auto insert_status = client->execute(insert, params);
      if (!ok(insert_status)) {
        step_ok = false;
        step_error = status_text(insert_status, client->last_error());
      }
    }
    if (!step_ok) {
      client->rollback();
      err = "migration " + std::to_string(step.version) + " (" + step.name +
            ") failed: " + step_error;
      return false;
    }
    if (!ok(client->commit())) {
      client->rollback();
      err = "migration " + std::to_string(step.version) +
            " could not commit: " + client->last_error();
      return false;
    }
  }

  const std::string seed =
      "INSERT INTO server_state (id) VALUES (1) ON CONFLICT (id) DO NOTHING";
  status = client->execute(seed);
  if (!ok(status)) {
    err = status_text(status, client->last_error());
    return false;
  }
  if (!seed_defaults(err)) return false;
  log_info("database: schema version %d", schema_version());
  return true;
}

bool database::seed_defaults(std::string& err) {
  // Runtime settings live in the settings table; the JSON blobs are owned by
  // the settings repository, which is added in the next milestone. Seed only
  // the row that makes the bootstrap state explicit.
  (void)err;
  return true;
}

void database::close() { pool_.close(); }

uvcpp::uvcpp_db_client* database::acquire(int timeout_ms) {
  return pool_.acquire(timeout_ms);
}

void database::release(uvcpp::uvcpp_db_client* client) {
  if (client != nullptr) pool_.release(client);
}

void database::discard(uvcpp::uvcpp_db_client* client) {
  if (client != nullptr) pool_.discard(client);
}

int database::schema_version() const {
  auto* self = const_cast<database*>(this);
  client_guard guard(*self, 5000);
  if (!guard) return 0;
  uvcpp::uvcpp_db_table table;
  const auto status = guard.get()->query(
      "SELECT COALESCE(MAX(version), 0) AS v FROM schema_migrations", &table);
  if (!ok(status)) return 0;
  return static_cast<int>(table.value("v").to_int64());
}

bool database::healthy() const {
  return ok(const_cast<database*>(this)->pool_.ping());
}

std::string database::last_error() const { return pool_.last_error(); }

}  // namespace molink
