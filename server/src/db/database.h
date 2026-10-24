#pragma once

#include <string>

#include <db/uvcpp_db_pool.h>

#include "config.h"
#include "db/sql_dialect.h"

namespace molink {

/// Owns the libuvcpp connection pool and the schema migrations. All access
/// from request handlers must go through a worker thread; the synchronous
/// acquire/release pair here is for startup and maintenance only.
class database {
 public:
  explicit database(database_config config);
  ~database();

  database(const database&) = delete;
  database& operator=(const database&) = delete;

  bool open(std::string& err);
  bool migrate(std::string& err);
  void close();

  uvcpp::uvcpp_db_pool& pool() { return pool_; }
  uvcpp::uvcpp_db_client* acquire(int timeout_ms = 5000);
  void release(uvcpp::uvcpp_db_client* client);
  void discard(uvcpp::uvcpp_db_client* client);

  const sql_dialect& dialect() const { return dialect_; }
  const database_config& config() const { return config_; }
  const std::string& url() const { return url_; }
  int schema_version() const;
  bool healthy() const;
  std::string last_error() const;

 private:
  bool apply_sqlite_settings(std::string& err);
  bool seed_defaults(std::string& err);

  database_config config_;
  sql_dialect dialect_;
  uvcpp::uvcpp_db_pool pool_;
  std::string url_;
};

}  // namespace molink
