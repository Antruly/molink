#pragma once

#include <string>

namespace molink {

enum class db_driver { sqlite, postgres };

inline db_driver parse_db_driver(const std::string& name, bool& ok) {
  if (name == "sqlite") {
    ok = true;
    return db_driver::sqlite;
  }
  if (name == "postgres" || name == "postgresql") {
    ok = true;
    return db_driver::postgres;
  }
  ok = false;
  return db_driver::sqlite;
}

inline const char* db_driver_name(db_driver driver) {
  return driver == db_driver::postgres ? "postgres" : "sqlite";
}

/// libuvcpp's DB layer hands SQL to the driver verbatim, so placeholders and a
/// few DDL details are dialect-specific. Everything else in the schema is
/// written in the common subset.
class sql_dialect {
 public:
  explicit sql_dialect(db_driver driver) : driver_(driver) {}

  db_driver driver() const { return driver_; }

  /// One-based, matching PostgreSQL's `$n`; SQLite returns `?`.
  std::string placeholder(int one_based) const {
    if (driver_ == db_driver::postgres) {
      return "$" + std::to_string(one_based);
    }
    return "?";
  }

  std::string autoincrement_pk(const std::string& column) const {
    if (driver_ == db_driver::postgres) {
      return column + " BIGSERIAL PRIMARY KEY";
    }
    return column + " INTEGER PRIMARY KEY AUTOINCREMENT";
  }

 private:
  db_driver driver_;
};

}  // namespace molink
