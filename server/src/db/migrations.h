#pragma once

#include <string>
#include <vector>

namespace molink {

struct migration {
  int version = 0;
  std::string name;
  std::vector<std::string> sqlite_sql;
  std::vector<std::string> postgres_sql;
};

/// Ordered, append-only. Every statement is a single SQL statement without a
/// trailing semicolon (libuvcpp passes SQL through unchanged).
const std::vector<migration>& schema_migrations();

}  // namespace molink
