#include "auth/auth_service.h"

#include "auth/password.h"
#include "util.h"

namespace molink {
namespace {

std::string new_session_id() {
  const std::string hex = random_hex(12);
  return hex.empty() ? std::string() : "s_" + hex;
}

std::string new_session_token() { return random_hex(32); }

std::string csrf_token() { return random_hex(32); }

void set_error(auth_error& error, auth_code code, const std::string& text) {
  error.code = code;
  error.message = text;
}

}  // namespace

const char* auth_code_name(auth_code code) {
  switch (code) {
    case auth_code::none:
      return "ok";
    case auth_code::invalid_credentials:
      return "invalid_credentials";
    case auth_code::disabled:
      return "disabled";
    case auth_code::unauthorized:
      return "unauthorized";
    case auth_code::setup_closed:
      return "setup_closed";
    case auth_code::conflict:
      return "conflict";
    case auth_code::not_found:
      return "not_found";
    case auth_code::weak_password:
      return "weak_password";
    case auth_code::invalid_username:
      return "invalid_username";
    case auth_code::invalid_role:
      return "invalid_role";
    case auth_code::last_admin:
      return "last_admin";
    case auth_code::self_delete:
      return "self_delete";
    case auth_code::internal:
      return "internal_error";
  }
  return "internal_error";
}

auth_service::auth_service(user_repository& users,
                           session_repository& sessions,
                           const session_policy& policy)
    : users_(users), sessions_(sessions), policy_(policy) {}

bool auth_service::valid_username(const std::string& username) {
  if (username.size() < 3 || username.size() > 64) return false;
  for (unsigned char c : username) {
    const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                         c == '-';
    if (!allowed) return false;
  }
  return true;
}

bool auth_service::valid_role(const std::string& role) {
  return role == "admin" || role == "operator" || role == "viewer";
}

bool auth_service::setup_required(uvcpp::uvcpp_db_client& client,
                                  const sql_dialect& dialect, bool& out,
                                  std::string& err) const {
  std::size_t count = 0;
  if (!users_.count_all(client, dialect, count, err)) return false;
  out = count == 0;
  return true;
}

bool auth_service::issue_session(uvcpp::uvcpp_db_client& client,
                                 const sql_dialect& dialect,
                                 const admin_user& user, const std::string& ip,
                                 const std::string& user_agent, login_result& out,
                                 std::string& err) const {
  const std::string raw = new_session_token();
  if (raw.empty()) {
    err = "secure random generator is unavailable";
    return false;
  }
  const std::uint64_t now = epoch_ms();
  session_record session;
  session.id = new_session_id();
  session.token_sha256 = sha256_hex(raw);
  session.user_id = user.id;
  session.created_ms = now;
  session.last_seen_ms = now;
  session.expires_ms = now + policy_.absolute_ms;
  session.ip = ip;
  session.user_agent = user_agent;
  if (!sessions_.create(client, dialect, session, err)) return false;

  out.session_token = raw;
  out.csrf_token = csrf_token();
  out.session_id = session.id;
  out.user_id = user.id;
  out.username = user.username;
  out.display_name = user.display_name;
  out.role = user.role;
  out.expires_ms = session.expires_ms;
  return true;
}

bool auth_service::create_first_admin(
    uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
    const std::string& username, const std::string& display_name,
    const std::string& password, const std::string& ip,
    const std::string& user_agent, login_result& out, auth_error& error) const {
  error = auth_error{};
  std::size_t count = 0;
  std::string err;
  if (!users_.count_all(client, dialect, count, err)) {
    set_error(error, auth_code::internal, err);
    return false;
  }
  if (count != 0) {
    set_error(error, auth_code::setup_closed, "the server is already set up");
    return false;
  }
  if (!valid_username(username)) {
    set_error(error, auth_code::invalid_username,
              "username must be 3-64 letters, digits, '.', '_' or '-'");
    return false;
  }
  if (!password_acceptable(password, err)) {
    set_error(error, auth_code::weak_password, err);
    return false;
  }
  password_hash hashed;
  if (!hash_password(password, hashed, err)) {
    set_error(error, auth_code::internal, err);
    return false;
  }
  admin_user user;
  user.id = "u_" + random_hex(8);
  user.username = username;
  user.username_norm = lowercase_ascii(username);
  user.display_name = display_name.empty() ? username : display_name;
  user.role = "admin";
  user.password_algo = hashed.algorithm;
  user.password_iterations = hashed.iterations;
  user.password_salt = hashed.salt_hex;
  user.password_hash = hashed.hash_hex;
  user.created_ms = epoch_ms();
  user.updated_ms = user.created_ms;
  if (!users_.create(client, dialect, user, err)) {
    set_error(error, auth_code::conflict, err);
    return false;
  }
  if (!issue_session(client, dialect, user, ip, user_agent, out, err)) {
    set_error(error, auth_code::internal, err);
    return false;
  }
  return true;
}

bool auth_service::login(uvcpp::uvcpp_db_client& client,
                         const sql_dialect& dialect,
                         const std::string& username,
                         const std::string& password, const std::string& ip,
                         const std::string& user_agent, login_result& out,
                         auth_error& error) const {
  error = auth_error{};
  admin_user user;
  bool found = false;
  std::string err;
  if (!users_.find_by_username(client, dialect, username, user, found, err)) {
    set_error(error, auth_code::internal, err);
    return false;
  }
  if (!found) {
    // Equalise the timing with the real path; never reveal which half failed.
    verify_password(password, dummy_password_hash());
    set_error(error, auth_code::invalid_credentials,
              "the username or password is not valid");
    return false;
  }
  const password_hash stored{user.password_algo, user.password_iterations,
                             user.password_salt, user.password_hash};
  const bool matches = verify_password(password, stored);
  if (!matches || user.disabled) {
    set_error(error, auth_code::invalid_credentials,
              "the username or password is not valid");
    return false;
  }
  if (!users_.touch_login(client, dialect, user.id, epoch_ms())) {
    set_error(error, auth_code::internal, "could not update the login time");
    return false;
  }
  if (!issue_session(client, dialect, user, ip, user_agent, out, err)) {
    set_error(error, auth_code::internal, err);
    return false;
  }
  return true;
}

bool auth_service::validate_session(uvcpp::uvcpp_db_client& client,
                                    const sql_dialect& dialect,
                                    const std::string& raw_token,
                                    admin_user& user, session_record& session,
                                    auth_error& error) const {
  error = auth_error{};
  if (raw_token.empty()) {
    set_error(error, auth_code::unauthorized, "missing session token");
    return false;
  }
  bool found = false;
  std::string err;
  if (!sessions_.find_by_token(client, dialect, sha256_hex(raw_token), session,
                               found, err)) {
    set_error(error, auth_code::internal, err);
    return false;
  }
  const std::uint64_t now = epoch_ms();
  if (!found || session.expires_ms <= now ||
      now > session.last_seen_ms + policy_.idle_ms) {
    if (found) sessions_.remove(client, dialect, session.id);
    set_error(error, auth_code::unauthorized, "the session has expired");
    return false;
  }
  if (!users_.find_by_id(client, dialect, session.user_id, user, found, err)) {
    set_error(error, auth_code::internal, err);
    return false;
  }
  if (!found || user.disabled) {
    sessions_.remove(client, dialect, session.id);
    set_error(error, auth_code::unauthorized, "the account is unavailable");
    return false;
  }
  if (now > session.last_seen_ms + 60000) {
    const std::uint64_t absolute_deadline = session.created_ms + policy_.absolute_ms;
    const std::uint64_t idle_deadline = now + policy_.idle_ms;
    const std::uint64_t expires =
        absolute_deadline < idle_deadline ? absolute_deadline : idle_deadline;
    sessions_.touch(client, dialect, session.id, now, expires);
    session.last_seen_ms = now;
    session.expires_ms = expires;
  }
  return true;
}

bool auth_service::logout(uvcpp::uvcpp_db_client& client,
                          const sql_dialect& dialect,
                          const std::string& raw_token) const {
  if (raw_token.empty()) return true;
  session_record session;
  bool found = false;
  std::string err;
  if (!sessions_.find_by_token(client, dialect, sha256_hex(raw_token), session,
                               found, err)) {
    return false;
  }
  return !found || sessions_.remove(client, dialect, session.id);
}

bool auth_service::change_password(uvcpp::uvcpp_db_client& client,
                                   const sql_dialect& dialect,
                                   const admin_user& user,
                                   const std::string& current_password,
                                   const std::string& new_password,
                                   auth_error& error) const {
  error = auth_error{};
  const password_hash stored{user.password_algo, user.password_iterations,
                             user.password_salt, user.password_hash};
  if (!verify_password(current_password, stored)) {
    set_error(error, auth_code::invalid_credentials,
              "the current password is not valid");
    return false;
  }
  if (!password_acceptable(new_password, error.message)) {
    set_error(error, auth_code::weak_password, error.message);
    return false;
  }
  password_hash hashed;
  std::string err;
  if (!hash_password(new_password, hashed, err)) {
    set_error(error, auth_code::internal, err);
    return false;
  }
  if (!users_.update_password(client, dialect, user.id, hashed.algorithm,
                              hashed.iterations, hashed.salt_hex,
                              hashed.hash_hex, err)) {
    set_error(error, auth_code::internal, err);
    return false;
  }
  return true;
}

bool auth_service::create_user(uvcpp::uvcpp_db_client& client,
                               const sql_dialect& dialect,
                               const std::string& username,
                               const std::string& display_name,
                               const std::string& role,
                               const std::string& password, admin_user& out,
                               auth_error& error) const {
  error = auth_error{};
  if (!valid_username(username)) {
    set_error(error, auth_code::invalid_username,
              "username must be 3-64 letters, digits, '.', '_' or '-'");
    return false;
  }
  if (!valid_role(role)) {
    set_error(error, auth_code::invalid_role, "unknown role");
    return false;
  }
  std::string err;
  if (!password_acceptable(password, err)) {
    set_error(error, auth_code::weak_password, err);
    return false;
  }
  password_hash hashed;
  if (!hash_password(password, hashed, err)) {
    set_error(error, auth_code::internal, err);
    return false;
  }
  admin_user user;
  user.id = "u_" + random_hex(8);
  user.username = username;
  user.username_norm = lowercase_ascii(username);
  user.display_name = display_name.empty() ? username : display_name;
  user.role = role;
  user.password_algo = hashed.algorithm;
  user.password_iterations = hashed.iterations;
  user.password_salt = hashed.salt_hex;
  user.password_hash = hashed.hash_hex;
  user.created_ms = epoch_ms();
  user.updated_ms = user.created_ms;
  if (!users_.create(client, dialect, user, err)) {
    set_error(error, auth_code::conflict, err);
    return false;
  }
  out = user;
  return true;
}

bool auth_service::find_user(uvcpp::uvcpp_db_client& client,
                             const sql_dialect& dialect, const std::string& id,
                             admin_user& out, bool& found,
                             std::string& err) const {
  return users_.find_by_id(client, dialect, id, out, found, err);
}

bool auth_service::update_user(uvcpp::uvcpp_db_client& client,
                               const sql_dialect& dialect,
                               const admin_user& actor,
                               const std::string& target_id,
                               const std::string& display_name,
                               const std::string& role, bool disabled,
                               auth_error& error) const {
  error = auth_error{};
  if (!valid_role(role)) {
    set_error(error, auth_code::invalid_role, "unknown role");
    return false;
  }
  admin_user target;
  bool found = false;
  std::string err;
  if (!find_user(client, dialect, target_id, target, found, err)) {
    set_error(error, auth_code::internal, err);
    return false;
  }
  if (!found) {
    set_error(error, auth_code::not_found, "user not found");
    return false;
  }
  const bool loses_admin =
      target.role == "admin" && (role != "admin" || disabled);
  if (loses_admin) {
    if (target.id == actor.id) {
      set_error(error, auth_code::last_admin,
                "you cannot remove your own administrator role");
      return false;
    }
    std::size_t admins = 0;
    if (!users_.count_enabled_admins(client, dialect, admins, err)) {
      set_error(error, auth_code::internal, err);
      return false;
    }
    if (admins <= 1) {
      set_error(error, auth_code::last_admin,
                "at least one enabled administrator must remain");
      return false;
    }
  }
  if (!users_.update_profile(client, dialect, target.id, display_name, role,
                             disabled, err)) {
    set_error(error, auth_code::internal, err);
    return false;
  }
  if (disabled || role != target.role) {
    sessions_.remove_for_user(client, dialect, target.id, "");
  }
  return true;
}

bool auth_service::delete_user(uvcpp::uvcpp_db_client& client,
                               const sql_dialect& dialect,
                               const admin_user& actor,
                               const std::string& target_id,
                               auth_error& error) const {
  error = auth_error{};
  if (target_id == actor.id) {
    set_error(error, auth_code::self_delete,
              "you cannot delete your own account");
    return false;
  }
  admin_user target;
  bool found = false;
  std::string err;
  if (!find_user(client, dialect, target_id, target, found, err)) {
    set_error(error, auth_code::internal, err);
    return false;
  }
  if (!found) {
    set_error(error, auth_code::not_found, "user not found");
    return false;
  }
  if (target.role == "admin" && !target.disabled) {
    std::size_t admins = 0;
    if (!users_.count_enabled_admins(client, dialect, admins, err)) {
      set_error(error, auth_code::internal, err);
      return false;
    }
    if (admins <= 1) {
      set_error(error, auth_code::last_admin,
                "at least one enabled administrator must remain");
      return false;
    }
  }
  bool removed = false;
  if (!users_.remove(client, dialect, target.id, removed, err)) {
    set_error(error, auth_code::internal, err);
    return false;
  }
  sessions_.remove_for_user(client, dialect, target.id, "");
  return true;
}

bool auth_service::reset_password(uvcpp::uvcpp_db_client& client,
                                  const sql_dialect& dialect,
                                  const std::string& target_id,
                                  const std::string& new_password,
                                  auth_error& error) const {
  error = auth_error{};
  admin_user target;
  bool found = false;
  std::string err;
  if (!find_user(client, dialect, target_id, target, found, err)) {
    set_error(error, auth_code::internal, err);
    return false;
  }
  if (!found) {
    set_error(error, auth_code::not_found, "user not found");
    return false;
  }
  if (!password_acceptable(new_password, err)) {
    set_error(error, auth_code::weak_password, err);
    return false;
  }
  password_hash hashed;
  if (!hash_password(new_password, hashed, err)) {
    set_error(error, auth_code::internal, err);
    return false;
  }
  if (!users_.update_password(client, dialect, target.id, hashed.algorithm,
                              hashed.iterations, hashed.salt_hex,
                              hashed.hash_hex, err)) {
    set_error(error, auth_code::internal, err);
    return false;
  }
  sessions_.remove_for_user(client, dialect, target.id, "");
  return true;
}

}  // namespace molink
