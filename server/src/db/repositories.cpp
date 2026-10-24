#include "db/repositories.h"

#include <algorithm>
#include <cctype>
#include <utility>

#include "security/ip.h"
#include "settings.h"
#include "util.h"

namespace molink {
namespace {

bool ok(uvcpp::uvcpp_db_status status) { return uvcpp::uvcpp_db_ok(status); }

std::string error_of(uvcpp::uvcpp_db_client& client,
                     uvcpp::uvcpp_db_status status) {
  std::string text = uvcpp::uvcpp_db_status_name(status);
  if (!client.last_error().empty()) {
    text += ": ";
    text += client.last_error();
  }
  return text;
}

bool exec(uvcpp::uvcpp_db_client& client, const std::string& sql,
          const uvcpp::uvcpp_db_params& params, std::string& err,
          int64_t* affected = nullptr) {
  const auto status = client.execute(sql, params, affected);
  if (ok(status)) return true;
  err = error_of(client, status);
  return false;
}

bool exec(uvcpp::uvcpp_db_client& client, const std::string& sql,
          std::string& err, int64_t* affected = nullptr) {
  const auto status = client.execute(sql, affected);
  if (ok(status)) return true;
  err = error_of(client, status);
  return false;
}

bool query(uvcpp::uvcpp_db_client& client, const std::string& sql,
           const uvcpp::uvcpp_db_params& params, uvcpp::uvcpp_db_table& table,
           std::string& err) {
  const auto status = client.query(sql, params, &table);
  if (ok(status)) return true;
  err = error_of(client, status);
  return false;
}

std::string random_id(const char* prefix, std::size_t bytes) {
  const std::string hex = random_hex(bytes);
  return hex.empty() ? std::string() : std::string(prefix) + hex;
}

std::string device_id() {
  static const char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  static constexpr std::size_t kSize = sizeof(kAlphabet) - 1;
  const std::string bytes = random_bytes(9);
  if (bytes.size() != 9) return std::string();
  std::string out;
  out.reserve(9);
  for (unsigned char byte : bytes) out.push_back(kAlphabet[byte % kSize]);
  return out;
}

std::string search_key(const std::string& name, const std::string& id,
                       const std::string& peer_ip) {
  return lowercase_ascii(name + " " + id + " " + peer_ip);
}

void fill_device(const uvcpp::uvcpp_db_row& row, device_record& out) {
  out.id = row["id"].to_text();
  out.token_hash = row["token_hash"].to_text();
  out.name = row["name"].to_text();
  out.address = row["address"].to_text();
  out.peer_ip = row["peer_ip"].to_text();
  out.port = static_cast<int>(row["port"].to_int64());
  out.version = row["version"].to_text();
  out.created_ms = static_cast<std::uint64_t>(row["created_ms"].to_int64());
  out.last_seen_ms = static_cast<std::uint64_t>(row["last_seen_ms"].to_int64());
}

void fill_user(const uvcpp::uvcpp_db_row& row, admin_user& out) {
  out.id = row["id"].to_text();
  out.username = row["username"].to_text();
  out.username_norm = row["username_norm"].to_text();
  out.display_name = row["display_name"].to_text();
  out.role = row["role"].to_text();
  out.password_algo = row["password_algo"].to_text();
  out.password_iterations = static_cast<int>(row["password_iterations"].to_int64());
  out.password_salt = row["password_salt"].to_text();
  out.password_hash = row["password_hash"].to_text();
  out.created_ms = static_cast<std::uint64_t>(row["created_ms"].to_int64());
  out.updated_ms = static_cast<std::uint64_t>(row["updated_ms"].to_int64());
  out.last_login_ms =
      static_cast<std::uint64_t>(row["last_login_ms"].to_int64());
  out.disabled = row["disabled"].to_int64() != 0;
  out.must_change_password = row["must_change_password"].to_int64() != 0;
}

void fill_session(const uvcpp::uvcpp_db_row& row, session_record& out) {
  out.id = row["id"].to_text();
  out.token_sha256 = row["token_sha256"].to_text();
  out.user_id = row["user_id"].to_text();
  out.created_ms = static_cast<std::uint64_t>(row["created_ms"].to_int64());
  out.last_seen_ms = static_cast<std::uint64_t>(row["last_seen_ms"].to_int64());
  out.expires_ms = static_cast<std::uint64_t>(row["expires_ms"].to_int64());
  out.ip = row["ip"].to_text();
  out.user_agent = row["user_agent"].to_text();
}

void fill_ban(const uvcpp::uvcpp_db_row& row, ban_record& out) {
  out.id = row["id"].to_text();
  out.target = row["target"].to_text();
  out.target_kind = row["target_kind"].to_text();
  out.reason = row["reason"].to_text();
  out.source = row["source"].to_text();
  out.created_ms = static_cast<std::uint64_t>(row["created_ms"].to_int64());
  out.expires_ms = static_cast<std::uint64_t>(row["expires_ms"].to_int64());
  out.created_by = row["created_by"].to_text();
  out.note = row["note"].to_text();
}

const char* kDeviceColumns =
    "id, token_hash, name, address, peer_ip, port, version, created_ms, "
    "last_seen_ms";
const char* kUserColumns =
    "id, username, username_norm, display_name, role, password_algo, "
    "password_iterations, password_salt, password_hash, created_ms, "
    "updated_ms, last_login_ms, disabled, must_change_password";

}  // namespace

// ---------------------------------------------------------------- devices ---

bool device_repository::insert(uvcpp::uvcpp_db_client& client,
                               const sql_dialect& dialect, device_record& record,
                               std::string& err) const {
  record.created_ms = record.created_ms == 0 ? epoch_ms() : record.created_ms;
  record.last_seen_ms =
      record.last_seen_ms == 0 ? record.created_ms : record.last_seen_ms;
  for (int attempt = 0; attempt < 100; ++attempt) {
    record.id = device_id();
    if (record.id.empty()) {
      err = "secure random generator is unavailable";
      return false;
    }
    const std::string sql =
        "INSERT INTO devices (id, token_hash, name, address, peer_ip, port, "
        "version, search_key, created_ms, last_seen_ms) VALUES (" +
        dialect.placeholder(1) + ", " + dialect.placeholder(2) + ", " +
        dialect.placeholder(3) + ", " + dialect.placeholder(4) + ", " +
        dialect.placeholder(5) + ", " + dialect.placeholder(6) + ", " +
        dialect.placeholder(7) + ", " + dialect.placeholder(8) + ", " +
        dialect.placeholder(9) + ", " + dialect.placeholder(10) + ")";
    const uvcpp::uvcpp_db_params params{
        record.id,
        record.token_hash,
        record.name,
        record.address,
        record.peer_ip,
        record.port,
        record.version,
        search_key(record.name, record.id, record.peer_ip),
        static_cast<int64_t>(record.created_ms),
        static_cast<int64_t>(record.last_seen_ms)};
    int64_t affected = 0;
    const auto status = client.execute(sql, params, &affected);
    if (ok(status)) return true;
    const std::string message = client.last_error();
    const bool collision =
        message.find("UNIQUE") != std::string::npos ||
        message.find("unique") != std::string::npos ||
        message.find("duplicate") != std::string::npos;
    if (!collision) {
      err = error_of(client, status);
      return false;
    }
  }
  err = "could not allocate a device id";
  return false;
}

bool device_repository::heartbeat(uvcpp::uvcpp_db_client& client,
                                  const sql_dialect& dialect,
                                  const std::string& id,
                                  const std::string& token_hash,
                                  const std::string& address,
                                  const std::string& peer_ip, int port,
                                  const std::string& version, bool& found,
                                  std::string& err) const {
  // NULLIF/COALESCE use each placeholder exactly once: SQLite binds one
  // parameter per `?`, while repeating `$2` would be fine on PostgreSQL only.
  const std::string sql =
      "UPDATE devices SET last_seen_ms = " + dialect.placeholder(1) +
      ", address = COALESCE(NULLIF(" + dialect.placeholder(2) +
      ", ''), address), peer_ip = " + dialect.placeholder(3) +
      ", port = " + dialect.placeholder(4) + ", version = COALESCE(NULLIF(" +
      dialect.placeholder(5) + ", ''), version) WHERE id = " +
      dialect.placeholder(6) + " AND token_hash = " + dialect.placeholder(7);
  const uvcpp::uvcpp_db_params params{
      static_cast<int64_t>(epoch_ms()), address, peer_ip, port, version, id,
      token_hash};
  int64_t affected = 0;
  if (!exec(client, sql, params, err, &affected)) return false;
  found = affected > 0;
  return true;
}

bool device_repository::remove(uvcpp::uvcpp_db_client& client,
                               const sql_dialect& dialect,
                               const std::string& id,
                               const std::string& token_hash, bool& found) const {
  std::string err;
  const std::string sql = "DELETE FROM devices WHERE id = " +
                          dialect.placeholder(1) + " AND token_hash = " +
                          dialect.placeholder(2);
  int64_t affected = 0;
  if (!exec(client, sql,
            uvcpp::uvcpp_db_params{id, token_hash}, err, &affected)) {
    return false;
  }
  found = affected > 0;
  return true;
}

bool device_repository::revoke(uvcpp::uvcpp_db_client& client,
                               const sql_dialect& dialect,
                               const std::string& id, bool& found) const {
  std::string err;
  const std::string sql =
      "DELETE FROM devices WHERE id = " + dialect.placeholder(1);
  int64_t affected = 0;
  if (!exec(client, sql, uvcpp::uvcpp_db_params{id}, err, &affected)) {
    return false;
  }
  found = affected > 0;
  return true;
}

bool device_repository::find(uvcpp::uvcpp_db_client& client,
                             const sql_dialect& dialect, const std::string& id,
                             device_record& out, std::string& err) const {
  uvcpp::uvcpp_db_table table;
  const std::string sql = std::string("SELECT ") + kDeviceColumns +
                          " FROM devices WHERE id = " + dialect.placeholder(1);
  if (!query(client, sql, uvcpp::uvcpp_db_params{id}, table, err)) return false;
  if (table.empty()) {
    err = "device not found";
    return false;
  }
  fill_device(table.rows().front(), out);
  return true;
}

bool device_repository::list(uvcpp::uvcpp_db_client& client,
                             const sql_dialect& dialect,
                             const device_query& query_in,
                             std::uint64_t offline_after_ms,
                             std::uint64_t now_ms, device_page& out,
                             std::string& err) const {
  std::vector<std::string> clauses;
  uvcpp::uvcpp_db_params params;
  auto add = [&](const std::string& clause, uvcpp::uvcpp_db_value value) {
    clauses.push_back(clause);
    params.add(std::move(value));
  };
  auto ph = [&]() {
    return dialect.placeholder(static_cast<int>(params.size()) + 1);
  };

  if (query_in.online == 1) {
    add("last_seen_ms >= " + ph(),
        static_cast<int64_t>(now_ms > offline_after_ms
                                 ? now_ms - offline_after_ms
                                 : 0));
  } else if (query_in.online == 0) {
    add("last_seen_ms < " + ph(),
        static_cast<int64_t>(now_ms > offline_after_ms
                                 ? now_ms - offline_after_ms
                                 : 0));
  }
  if (!query_in.search.empty()) {
    add("search_key LIKE " + ph(),
        std::string("%") + lowercase_ascii(query_in.search) + "%");
  }

  std::string where;
  if (!clauses.empty()) {
    where = " WHERE ";
    for (std::size_t i = 0; i < clauses.size(); ++i) {
      if (i != 0) where += " AND ";
      where += clauses[i];
    }
  }

  uvcpp::uvcpp_db_table count_table;
  if (!query(client,
             std::string("SELECT COUNT(*) AS n FROM devices") + where, params,
             count_table, err)) {
    return false;
  }
  out.total = static_cast<std::size_t>(count_table.value("n").to_int64());

  std::string order = "last_seen_ms DESC";
  if (query_in.sort == "created") {
    order = "created_ms DESC";
  } else if (query_in.sort == "name") {
    order = "name ASC";
  }

  std::size_t page = query_in.page == 0 ? 1 : query_in.page;
  std::size_t page_size = query_in.page_size == 0 ? 50 : query_in.page_size;
  page_size = std::min<std::size_t>(page_size, 200);
  uvcpp::uvcpp_db_params page_params = params;
  const std::string limit_ph =
      dialect.placeholder(static_cast<int>(page_params.size()) + 1);
  page_params.add(static_cast<int64_t>(page_size));
  const std::string offset_ph =
      dialect.placeholder(static_cast<int>(page_params.size()) + 1);
  page_params.add(static_cast<int64_t>((page - 1) * page_size));

  uvcpp::uvcpp_db_table table;
  const std::string sql = std::string("SELECT ") + kDeviceColumns +
                          " FROM devices" + where + " ORDER BY " + order +
                          " LIMIT " + limit_ph + " OFFSET " + offset_ph;
  if (!query(client, sql, page_params, table, err)) return false;
  out.items.clear();
  out.items.reserve(table.row_count());
  for (const uvcpp::uvcpp_db_row& row : table.rows()) {
    device_record record;
    fill_device(row, record);
    out.items.push_back(std::move(record));
  }
  return true;
}

bool device_repository::count_for_ip(uvcpp::uvcpp_db_client& client,
                                     const sql_dialect& dialect,
                                     const std::string& peer_ip,
                                     std::size_t& out, std::string& err) const {
  uvcpp::uvcpp_db_table table;
  const std::string sql = "SELECT COUNT(*) AS n FROM devices WHERE peer_ip = " +
                          dialect.placeholder(1);
  if (!query(client, sql, uvcpp::uvcpp_db_params{peer_ip}, table, err)) {
    return false;
  }
  out = static_cast<std::size_t>(table.value("n").to_int64());
  return true;
}

// ------------------------------------------------------------------ users ---

bool user_repository::create(uvcpp::uvcpp_db_client& client,
                             const sql_dialect& dialect,
                             const admin_user& user, std::string& err) const {
  const std::string sql =
      std::string("INSERT INTO admin_users (") + kUserColumns + ") VALUES (" +
      dialect.placeholder(1) + ", " + dialect.placeholder(2) + ", " +
      dialect.placeholder(3) + ", " + dialect.placeholder(4) + ", " +
      dialect.placeholder(5) + ", " + dialect.placeholder(6) + ", " +
      dialect.placeholder(7) + ", " + dialect.placeholder(8) + ", " +
      dialect.placeholder(9) + ", " + dialect.placeholder(10) + ", " +
      dialect.placeholder(11) + ", " + dialect.placeholder(12) + ", " +
      dialect.placeholder(13) + ", " + dialect.placeholder(14) + ")";
  const uvcpp::uvcpp_db_params params{
      user.id,
      user.username,
      lowercase_ascii(user.username),
      user.display_name,
      user.role,
      user.password_algo,
      user.password_iterations,
      user.password_salt,
      user.password_hash,
      static_cast<int64_t>(user.created_ms),
      static_cast<int64_t>(user.updated_ms),
      static_cast<int64_t>(user.last_login_ms),
      user.disabled ? 1 : 0,
      user.must_change_password ? 1 : 0};
  return exec(client, sql, params, err);
}

bool user_repository::find_by_username(uvcpp::uvcpp_db_client& client,
                                       const sql_dialect& dialect,
                                       const std::string& username,
                                       admin_user& out, bool& found,
                                       std::string& err) const {
  uvcpp::uvcpp_db_table table;
  const std::string sql = std::string("SELECT ") + kUserColumns +
                          " FROM admin_users WHERE username_norm = " +
                          dialect.placeholder(1);
  if (!query(client, sql,
             uvcpp::uvcpp_db_params{lowercase_ascii(username)}, table, err)) {
    return false;
  }
  found = !table.empty();
  if (found) fill_user(table.rows().front(), out);
  return true;
}

bool user_repository::find_by_id(uvcpp::uvcpp_db_client& client,
                                 const sql_dialect& dialect,
                                 const std::string& id, admin_user& out,
                                 bool& found, std::string& err) const {
  uvcpp::uvcpp_db_table table;
  const std::string sql = std::string("SELECT ") + kUserColumns +
                          " FROM admin_users WHERE id = " +
                          dialect.placeholder(1);
  if (!query(client, sql, uvcpp::uvcpp_db_params{id}, table, err)) return false;
  found = !table.empty();
  if (found) fill_user(table.rows().front(), out);
  return true;
}

bool user_repository::list(uvcpp::uvcpp_db_client& client,
                           const sql_dialect& dialect,
                           std::vector<admin_user>& out,
                           std::string& err) const {
  uvcpp::uvcpp_db_table table;
  const std::string sql = std::string("SELECT ") + kUserColumns +
                          " FROM admin_users ORDER BY created_ms ASC";
  if (!query(client, sql, uvcpp::uvcpp_db_params{}, table, err)) return false;
  out.clear();
  out.reserve(table.row_count());
  for (const uvcpp::uvcpp_db_row& row : table.rows()) {
    admin_user user;
    fill_user(row, user);
    out.push_back(std::move(user));
  }
  return true;
}

bool user_repository::update_profile(uvcpp::uvcpp_db_client& client,
                                     const sql_dialect& dialect,
                                     const std::string& id,
                                     const std::string& display_name,
                                     const std::string& role, bool disabled,
                                     std::string& err) const {
  const std::string sql =
      "UPDATE admin_users SET display_name = " + dialect.placeholder(1) +
      ", role = " + dialect.placeholder(2) + ", disabled = " +
      dialect.placeholder(3) + ", updated_ms = " + dialect.placeholder(4) +
      " WHERE id = " + dialect.placeholder(5);
  return exec(client, sql,
              uvcpp::uvcpp_db_params{display_name, role, disabled ? 1 : 0,
                                     static_cast<int64_t>(epoch_ms()), id},
              err);
}

bool user_repository::update_password(uvcpp::uvcpp_db_client& client,
                                      const sql_dialect& dialect,
                                      const std::string& id,
                                      const std::string& algo, int iterations,
                                      const std::string& salt,
                                      const std::string& hash,
                                      std::string& err) const {
  const std::string sql =
      "UPDATE admin_users SET password_algo = " + dialect.placeholder(1) +
      ", password_iterations = " + dialect.placeholder(2) +
      ", password_salt = " + dialect.placeholder(3) +
      ", password_hash = " + dialect.placeholder(4) +
      ", updated_ms = " + dialect.placeholder(5) +
      " WHERE id = " + dialect.placeholder(6);
  return exec(client, sql,
              uvcpp::uvcpp_db_params{algo, iterations, salt, hash,
                                     static_cast<int64_t>(epoch_ms()), id},
              err);
}

bool user_repository::remove(uvcpp::uvcpp_db_client& client,
                             const sql_dialect& dialect, const std::string& id,
                             bool& found, std::string& err) const {
  const std::string sql =
      "DELETE FROM admin_users WHERE id = " + dialect.placeholder(1);
  int64_t affected = 0;
  if (!exec(client, sql, uvcpp::uvcpp_db_params{id}, err, &affected)) {
    return false;
  }
  found = affected > 0;
  return true;
}

bool user_repository::count_enabled_admins(uvcpp::uvcpp_db_client& client,
                                           const sql_dialect& dialect,
                                           std::size_t& out,
                                           std::string& err) const {
  uvcpp::uvcpp_db_table table;
  const std::string sql =
      "SELECT COUNT(*) AS n FROM admin_users WHERE role = 'admin' AND "
      "disabled = 0";
  if (!query(client, sql, uvcpp::uvcpp_db_params{}, table, err)) return false;
  out = static_cast<std::size_t>(table.value("n").to_int64());
  return true;
}

bool user_repository::count_all(uvcpp::uvcpp_db_client& client,
                                const sql_dialect& dialect, std::size_t& out,
                                std::string& err) const {
  uvcpp::uvcpp_db_table table;
  if (!query(client, "SELECT COUNT(*) AS n FROM admin_users",
             uvcpp::uvcpp_db_params{}, table, err)) {
    return false;
  }
  out = static_cast<std::size_t>(table.value("n").to_int64());
  return true;
}

bool user_repository::touch_login(uvcpp::uvcpp_db_client& client,
                                  const sql_dialect& dialect,
                                  const std::string& id,
                                  std::uint64_t now_ms) const {
  std::string err;
  const std::string sql =
      "UPDATE admin_users SET last_login_ms = " + dialect.placeholder(1) +
      " WHERE id = " + dialect.placeholder(2);
  return exec(client, sql,
              uvcpp::uvcpp_db_params{static_cast<int64_t>(now_ms), id}, err);
}

// --------------------------------------------------------------- sessions ---

bool session_repository::create(uvcpp::uvcpp_db_client& client,
                                const sql_dialect& dialect,
                                const session_record& session,
                                std::string& err) const {
  const std::string sql =
      "INSERT INTO sessions (id, token_sha256, user_id, created_ms, "
      "last_seen_ms, expires_ms, ip, user_agent) VALUES (" +
      dialect.placeholder(1) + ", " + dialect.placeholder(2) + ", " +
      dialect.placeholder(3) + ", " + dialect.placeholder(4) + ", " +
      dialect.placeholder(5) + ", " + dialect.placeholder(6) + ", " +
      dialect.placeholder(7) + ", " + dialect.placeholder(8) + ")";
  const uvcpp::uvcpp_db_params params{
      session.id, session.token_sha256, session.user_id,
      static_cast<int64_t>(session.created_ms),
      static_cast<int64_t>(session.last_seen_ms),
      static_cast<int64_t>(session.expires_ms), session.ip,
      session.user_agent};
  return exec(client, sql, params, err);
}

bool session_repository::find_by_token(uvcpp::uvcpp_db_client& client,
                                       const sql_dialect& dialect,
                                       const std::string& token_sha256,
                                       session_record& out, bool& found,
                                       std::string& err) const {
  uvcpp::uvcpp_db_table table;
  const std::string sql =
      "SELECT id, token_sha256, user_id, created_ms, last_seen_ms, expires_ms, "
      "ip, user_agent FROM sessions WHERE token_sha256 = " +
      dialect.placeholder(1);
  if (!query(client, sql, uvcpp::uvcpp_db_params{token_sha256}, table, err)) {
    return false;
  }
  found = !table.empty();
  if (found) fill_session(table.rows().front(), out);
  return true;
}

bool session_repository::touch(uvcpp::uvcpp_db_client& client,
                               const sql_dialect& dialect,
                               const std::string& id, std::uint64_t seen_ms,
                               std::uint64_t expires_ms) const {
  std::string err;
  const std::string sql =
      "UPDATE sessions SET last_seen_ms = " + dialect.placeholder(1) +
      ", expires_ms = " + dialect.placeholder(2) +
      " WHERE id = " + dialect.placeholder(3);
  return exec(client, sql,
              uvcpp::uvcpp_db_params{static_cast<int64_t>(seen_ms),
                                     static_cast<int64_t>(expires_ms), id},
              err);
}

bool session_repository::remove(uvcpp::uvcpp_db_client& client,
                                const sql_dialect& dialect,
                                const std::string& id) const {
  std::string err;
  const std::string sql =
      "DELETE FROM sessions WHERE id = " + dialect.placeholder(1);
  return exec(client, sql, uvcpp::uvcpp_db_params{id}, err);
}

bool session_repository::remove_for_user(uvcpp::uvcpp_db_client& client,
                                         const sql_dialect& dialect,
                                         const std::string& user_id,
                                         const std::string& keep_id) const {
  std::string err;
  if (keep_id.empty()) {
    return exec(client,
                "DELETE FROM sessions WHERE user_id = " +
                    dialect.placeholder(1),
                uvcpp::uvcpp_db_params{user_id}, err);
  }
  return exec(client,
              "DELETE FROM sessions WHERE user_id = " + dialect.placeholder(1) +
                  " AND id <> " + dialect.placeholder(2),
              uvcpp::uvcpp_db_params{user_id, keep_id}, err);
}

bool session_repository::list_for_user(
    uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
    const std::string& user_id, std::vector<session_record>& out,
    std::string& err) const {
  uvcpp::uvcpp_db_table table;
  const std::string sql =
      "SELECT id, token_sha256, user_id, created_ms, last_seen_ms, expires_ms, "
      "ip, user_agent FROM sessions WHERE user_id = " +
      dialect.placeholder(1) + " ORDER BY last_seen_ms DESC";
  if (!query(client, sql, uvcpp::uvcpp_db_params{user_id}, table, err)) {
    return false;
  }
  out.clear();
  for (const uvcpp::uvcpp_db_row& row : table.rows()) {
    session_record record;
    fill_session(row, record);
    out.push_back(std::move(record));
  }
  return true;
}

bool session_repository::prune_expired(uvcpp::uvcpp_db_client& client,
                                       const sql_dialect& dialect,
                                       std::uint64_t now_ms) const {
  std::string err;
  return exec(client,
              "DELETE FROM sessions WHERE expires_ms <= " +
                  dialect.placeholder(1),
              uvcpp::uvcpp_db_params{static_cast<int64_t>(now_ms)}, err);
}

// --------------------------------------------------------------- settings ---

bool settings_repository::ensure_defaults(uvcpp::uvcpp_db_client& client,
                                          const sql_dialect& dialect,
                                          std::string& err) const {
  struct entry {
    const char* key;
    std::string (*value)();
  };
  const entry entries[] = {
      {"server", &default_server_settings_json},
      {"security", &default_security_settings_json},
      {"relay", &default_relay_settings_json},
  };
  for (const entry& item : entries) {
    std::string json;
    bool found = false;
    if (!get(client, dialect, item.key, json, found, err)) return false;
    if (!found && !put(client, dialect, item.key, item.value(), epoch_ms(), err)) {
      return false;
    }
  }
  return true;
}

bool settings_repository::get(uvcpp::uvcpp_db_client& client,
                              const sql_dialect& dialect,
                              const std::string& key, std::string& json,
                              bool& found, std::string& err) const {
  uvcpp::uvcpp_db_table table;
  const std::string sql =
      "SELECT value_json FROM settings WHERE key = " + dialect.placeholder(1);
  if (!query(client, sql, uvcpp::uvcpp_db_params{key}, table, err)) return false;
  found = !table.empty();
  if (found) json = table.value("value_json").to_text();
  return true;
}

bool settings_repository::put(uvcpp::uvcpp_db_client& client,
                              const sql_dialect& dialect,
                              const std::string& key, const std::string& json,
                              std::uint64_t now_ms, std::string& err) const {
  int64_t affected = 0;
  const std::string update =
      "UPDATE settings SET value_json = " + dialect.placeholder(1) +
      ", updated_ms = " + dialect.placeholder(2) +
      " WHERE key = " + dialect.placeholder(3);
  if (!exec(client, update,
            uvcpp::uvcpp_db_params{json, static_cast<int64_t>(now_ms), key},
            err, &affected)) {
    return false;
  }
  if (affected > 0) return true;
  const std::string insert =
      "INSERT INTO settings (key, value_json, updated_ms) VALUES (" +
      dialect.placeholder(1) + ", " + dialect.placeholder(2) + ", " +
      dialect.placeholder(3) + ")";
  return exec(client, insert,
              uvcpp::uvcpp_db_params{key, json, static_cast<int64_t>(now_ms)},
              err);
}

bool settings_repository::revision(uvcpp::uvcpp_db_client& client,
                                   const sql_dialect& dialect, std::int64_t& out,
                                   std::string& err) const {
  uvcpp::uvcpp_db_table table;
  if (!query(client,
             "SELECT settings_revision AS v FROM server_state WHERE id = 1",
             uvcpp::uvcpp_db_params{}, table, err)) {
    return false;
  }
  out = table.value("v").to_int64();
  return true;
}

bool settings_repository::bump_revision(uvcpp::uvcpp_db_client& client,
                                        const sql_dialect& dialect,
                                        std::int64_t& out,
                                        std::string& err) const {
  if (!exec(client,
            "UPDATE server_state SET settings_revision = settings_revision + 1 "
            "WHERE id = 1",
            err)) {
    return false;
  }
  return revision(client, dialect, out, err);
}

// ------------------------------------------------------------------ bans ---

bool ban_repository::add(uvcpp::uvcpp_db_client& client,
                         const sql_dialect& dialect, const ban_record& ban,
                         std::string& err) const {
  const std::string sql =
      "INSERT INTO bans (id, target, target_kind, reason, source, created_ms, "
      "expires_ms, created_by, note) VALUES (" +
      dialect.placeholder(1) + ", " + dialect.placeholder(2) + ", " +
      dialect.placeholder(3) + ", " + dialect.placeholder(4) + ", " +
      dialect.placeholder(5) + ", " + dialect.placeholder(6) + ", " +
      dialect.placeholder(7) + ", " + dialect.placeholder(8) + ", " +
      dialect.placeholder(9) + ")";
  const uvcpp::uvcpp_db_params params{
      ban.id, ban.target, ban.target_kind, ban.reason, ban.source,
      static_cast<int64_t>(ban.created_ms),
      static_cast<int64_t>(ban.expires_ms), ban.created_by, ban.note};
  return exec(client, sql, params, err);
}

bool ban_repository::remove(uvcpp::uvcpp_db_client& client,
                            const sql_dialect& dialect, const std::string& id,
                            bool& found) const {
  std::string err;
  int64_t affected = 0;
  const std::string sql =
      "DELETE FROM bans WHERE id = " + dialect.placeholder(1);
  if (!exec(client, sql, uvcpp::uvcpp_db_params{id}, err, &affected)) {
    return false;
  }
  found = affected > 0;
  return true;
}

bool ban_repository::list(uvcpp::uvcpp_db_client& client,
                          const sql_dialect& dialect, std::size_t offset,
                          std::size_t limit, std::vector<ban_record>& out,
                          std::size_t& total, std::string& err) const {
  uvcpp::uvcpp_db_table count_table;
  if (!query(client, "SELECT COUNT(*) AS n FROM bans",
             uvcpp::uvcpp_db_params{}, count_table, err)) {
    return false;
  }
  total = static_cast<std::size_t>(count_table.value("n").to_int64());
  const std::string sql =
      "SELECT id, target, target_kind, reason, source, created_ms, expires_ms, "
      "created_by, note FROM bans ORDER BY created_ms DESC LIMIT " +
      dialect.placeholder(1) + " OFFSET " + dialect.placeholder(2);
  uvcpp::uvcpp_db_table table;
  if (!query(client, sql,
             uvcpp::uvcpp_db_params{static_cast<int64_t>(limit),
                                    static_cast<int64_t>(offset)},
             table, err)) {
    return false;
  }
  out.clear();
  for (const uvcpp::uvcpp_db_row& row : table.rows()) {
    ban_record record;
    fill_ban(row, record);
    out.push_back(std::move(record));
  }
  return true;
}

bool ban_repository::match(uvcpp::uvcpp_db_client& client,
                           const sql_dialect& dialect, const ip_addr& address,
                           std::uint64_t now_ms, ban_record& out, bool& found,
                           std::string& err) const {
  const std::string sql =
      "SELECT id, target, target_kind, reason, source, created_ms, expires_ms, "
      "created_by, note FROM bans WHERE expires_ms = 0 OR expires_ms > " +
      dialect.placeholder(1);
  uvcpp::uvcpp_db_table table;
  if (!query(client, sql, uvcpp::uvcpp_db_params{static_cast<int64_t>(now_ms)},
             table, err)) {
    return false;
  }
  found = false;
  for (const uvcpp::uvcpp_db_row& row : table.rows()) {
    ban_record record;
    fill_ban(row, record);
    if (record.target.find('/') != std::string::npos) {
      cidr_block block;
      if (parse_cidr(record.target, block, nullptr) &&
          cidr_contains(block, address)) {
        out = record;
        found = true;
        return true;
      }
    } else {
      ip_addr target;
      if (parse_ip(record.target, target, nullptr) &&
          canonical_ip(target) == canonical_ip(address)) {
        out = record;
        found = true;
        return true;
      }
    }
  }
  return true;
}

bool ban_repository::prune_expired(uvcpp::uvcpp_db_client& client,
                                   const sql_dialect& dialect,
                                   std::uint64_t now_ms) const {
  std::string err;
  return exec(client,
              "DELETE FROM bans WHERE expires_ms > 0 AND expires_ms <= " +
                  dialect.placeholder(1),
              uvcpp::uvcpp_db_params{static_cast<int64_t>(now_ms)}, err);
}

// ----------------------------------------------------------------- audit ---

bool audit_repository::append(uvcpp::uvcpp_db_client& client,
                              const sql_dialect& dialect,
                              const audit_record& record,
                              std::string& err) const {
  const std::string sql =
      "INSERT INTO audit_events (ts_ms, event, result, actor_id, actor_name, "
      "role, ip, target, details_json) VALUES (" +
      dialect.placeholder(1) + ", " + dialect.placeholder(2) + ", " +
      dialect.placeholder(3) + ", " + dialect.placeholder(4) + ", " +
      dialect.placeholder(5) + ", " + dialect.placeholder(6) + ", " +
      dialect.placeholder(7) + ", " + dialect.placeholder(8) + ", " +
      dialect.placeholder(9) + ")";
  const uvcpp::uvcpp_db_params params{
      static_cast<int64_t>(record.ts_ms), record.event, record.result,
      record.actor_id, record.actor_name, record.role, record.ip, record.target,
      record.details_json};
  return exec(client, sql, params, err);
}

bool audit_repository::list(uvcpp::uvcpp_db_client& client,
                            const sql_dialect& dialect, std::size_t offset,
                            std::size_t limit,
                            std::vector<audit_record>& out, std::size_t& total,
                            std::string& err) const {
  uvcpp::uvcpp_db_table count_table;
  if (!query(client, "SELECT COUNT(*) AS n FROM audit_events",
             uvcpp::uvcpp_db_params{}, count_table, err)) {
    return false;
  }
  total = static_cast<std::size_t>(count_table.value("n").to_int64());
  const std::string sql =
      "SELECT id, ts_ms, event, result, actor_id, actor_name, role, ip, "
      "target, details_json FROM audit_events ORDER BY ts_ms DESC LIMIT " +
      dialect.placeholder(1) + " OFFSET " + dialect.placeholder(2);
  uvcpp::uvcpp_db_table table;
  if (!query(client, sql,
             uvcpp::uvcpp_db_params{static_cast<int64_t>(limit),
                                    static_cast<int64_t>(offset)},
             table, err)) {
    return false;
  }
  out.clear();
  for (const uvcpp::uvcpp_db_row& row : table.rows()) {
    audit_record record;
    record.id = row["id"].to_int64();
    record.ts_ms = static_cast<std::uint64_t>(row["ts_ms"].to_int64());
    record.event = row["event"].to_text();
    record.result = row["result"].to_text();
    record.actor_id = row["actor_id"].to_text();
    record.actor_name = row["actor_name"].to_text();
    record.role = row["role"].to_text();
    record.ip = row["ip"].to_text();
    record.target = row["target"].to_text();
    record.details_json = row["details_json"].to_text();
    out.push_back(std::move(record));
  }
  return true;
}

bool audit_repository::prune_older_than(uvcpp::uvcpp_db_client& client,
                                        const sql_dialect& dialect,
                                        std::uint64_t cutoff_ms) const {
  std::string err;
  return exec(client,
              "DELETE FROM audit_events WHERE ts_ms < " +
                  dialect.placeholder(1),
              uvcpp::uvcpp_db_params{static_cast<int64_t>(cutoff_ms)}, err);
}

}  // namespace molink
