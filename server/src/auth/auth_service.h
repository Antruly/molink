#pragma once

#include <cstdint>
#include <string>

#include <db/uvcpp_db.h>

#include "db/repositories.h"

namespace molink {

struct session_policy {
  std::uint64_t idle_ms = 30u * 60u * 1000u;
  std::uint64_t absolute_ms = 12u * 60u * 60u * 1000u;
};

enum class auth_code {
  none = 0,
  invalid_credentials,
  disabled,
  unauthorized,
  setup_closed,
  conflict,
  not_found,
  weak_password,
  invalid_username,
  invalid_role,
  last_admin,
  self_delete,
  internal,
};

struct auth_error {
  auth_code code = auth_code::none;
  std::string message;

  bool ok() const { return code == auth_code::none; }
};

const char* auth_code_name(auth_code code);

struct login_result {
  std::string session_token;  // returned to the client once
  std::string csrf_token;
  std::string session_id;
  std::string user_id;
  std::string username;
  std::string display_name;
  std::string role;
  std::uint64_t expires_ms = 0;
};

/// Credential and admin-account logic. Every method is synchronous and takes a
/// borrowed connection; callers wrap it in db_executor and choose read/write.
class auth_service {
 public:
  auth_service(user_repository& users, session_repository& sessions,
               const session_policy& policy);

  bool setup_required(uvcpp::uvcpp_db_client& client,
                      const sql_dialect& dialect, bool& out,
                      std::string& err) const;

  /// Creates the first administrator and its session. The caller must run this
  /// in a write transaction; the user-count check is part of it.
  bool create_first_admin(uvcpp::uvcpp_db_client& client,
                          const sql_dialect& dialect, const std::string& username,
                          const std::string& display_name,
                          const std::string& password, const std::string& ip,
                          const std::string& user_agent, login_result& out,
                          auth_error& error) const;

  bool login(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
             const std::string& username, const std::string& password,
             const std::string& ip, const std::string& user_agent,
             login_result& out, auth_error& error) const;

  bool validate_session(uvcpp::uvcpp_db_client& client,
                        const sql_dialect& dialect, const std::string& raw_token,
                        admin_user& user, session_record& session,
                        auth_error& error) const;

  bool logout(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
              const std::string& raw_token) const;

  bool change_password(uvcpp::uvcpp_db_client& client,
                       const sql_dialect& dialect, const admin_user& user,
                       const std::string& current_password,
                       const std::string& new_password, auth_error& error) const;

  bool create_user(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
                   const std::string& username,
                   const std::string& display_name, const std::string& role,
                   const std::string& password, admin_user& out,
                   auth_error& error) const;

  bool update_user(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
                   const admin_user& actor, const std::string& target_id,
                   const std::string& display_name, const std::string& role,
                   bool disabled, auth_error& error) const;

  bool delete_user(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
                   const admin_user& actor, const std::string& target_id,
                   auth_error& error) const;

  /// Administrator password reset: all sessions of the target are revoked.
  bool reset_password(uvcpp::uvcpp_db_client& client,
                      const sql_dialect& dialect, const std::string& target_id,
                      const std::string& new_password, auth_error& error) const;

  static bool valid_username(const std::string& username);
  static bool valid_role(const std::string& role);

  /// Called after settings load, before the listeners start.
  void set_policy(const session_policy& policy) { policy_ = policy; }

 private:
  bool issue_session(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
                     const admin_user& user, const std::string& ip,
                     const std::string& user_agent, login_result& out,
                     std::string& err) const;
  bool find_user(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
                 const std::string& id, admin_user& out, bool& found,
                 std::string& err) const;

  user_repository& users_;
  session_repository& sessions_;
  session_policy policy_;
};

}  // namespace molink
