#include <cstdio>
#include <filesystem>

#include "auth/auth_service.h"
#include "db/database.h"
#include "db/repositories.h"
#include "util.h"

namespace {

int failures = 0;

#define CHECK(condition)                                              \
  do {                                                                \
    if (!(condition)) {                                               \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,    \
                   #condition);                                       \
      ++failures;                                                     \
    }                                                                 \
  } while (0)

}  // namespace

int main() {
  using namespace molink;
  std::error_code ec;
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path(ec) / "molink-server-auth-test";
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);

  database_config config;
  config.driver = "sqlite";
  config.url = sqlite_url_for(dir.string());
  config.pool_min = 1;
  config.pool_max = 2;
  config.busy_timeout_ms = 2000;

  database db(config);
  std::string err;
  CHECK(db.open(err));
  CHECK(db.migrate(err));
  if (failures != 0) return 1;

  const sql_dialect& dialect = db.dialect();
  user_repository users;
  session_repository sessions;
  session_policy policy;
  auth_service auth(users, sessions, policy);

  uvcpp::uvcpp_db_client* client = db.acquire();
  CHECK(client != nullptr);
  if (client == nullptr) return 1;

  bool setup = false;
  CHECK(auth.setup_required(*client, dialect, setup, err));
  CHECK(setup);

  login_result admin_login;
  auth_error error;
  CHECK(client->begin() == uvcpp::uvcpp_db_status::OK);
  CHECK(auth.create_first_admin(*client, dialect, "admin", "Administrator",
                                "correct horse battery", "127.0.0.1", "test",
                                admin_login, error));
  CHECK(client->commit() == uvcpp::uvcpp_db_status::OK);
  CHECK(error.ok());
  CHECK(!admin_login.session_token.empty());
  CHECK(admin_login.role == "admin");

  CHECK(auth.setup_required(*client, dialect, setup, err));
  CHECK(!setup);

  // A second setup attempt is closed.
  login_result duplicate;
  CHECK(!auth.create_first_admin(*client, dialect, "other", "Other",
                                 "correct horse battery", "", "", duplicate,
                                 error));
  CHECK(error.code == auth_code::setup_closed);

  admin_user session_user;
  session_record session;
  CHECK(auth.validate_session(*client, dialect, admin_login.session_token,
                              session_user, session, error));
  CHECK(session_user.role == "admin");

  login_result bad;
  CHECK(!auth.login(*client, dialect, "admin", "wrong password", "", "", bad,
                    error));
  CHECK(error.code == auth_code::invalid_credentials);

  login_result second;
  CHECK(auth.login(*client, dialect, "admin", "correct horse battery", "", "",
                   second, error));
  CHECK(error.ok());

  admin_user created;
  CHECK(client->begin() == uvcpp::uvcpp_db_status::OK);
  CHECK(auth.create_user(*client, dialect, "operator", "Operator", "operator",
                         "another strong password", created, error));
  CHECK(client->commit() == uvcpp::uvcpp_db_status::OK);
  CHECK(error.ok());

  login_result operator_login;
  CHECK(auth.login(*client, dialect, "operator", "another strong password", "",
                   "", operator_login, error));
  CHECK(error.ok() && operator_login.role == "operator");

  CHECK(auth.update_user(*client, dialect, session_user, created.id, "Operator",
                         "viewer", false, error));
  CHECK(error.ok());  // role change is allowed
  CHECK(!auth.delete_user(*client, dialect, session_user, session_user.id, error));
  CHECK(error.code == auth_code::self_delete);

  // Demoting the only administrator is refused.
  CHECK(!auth.update_user(*client, dialect, session_user, session_user.id,
                          "Administrator", "viewer", false, error));
  CHECK(error.code == auth_code::last_admin);

  CHECK(auth.change_password(*client, dialect, session_user,
                             "correct horse battery", "new correct horse battery",
                             error));
  CHECK(error.ok());
  CHECK(!auth.login(*client, dialect, "admin", "correct horse battery", "", "",
                    bad, error));
  CHECK(error.code == auth_code::invalid_credentials);
  CHECK(auth.login(*client, dialect, "admin", "new correct horse battery", "", "",
                   second, error));

  CHECK(auth.logout(*client, dialect, operator_login.session_token));
  admin_user after_logout;
  session_record session_after;
  CHECK(!auth.validate_session(*client, dialect, operator_login.session_token,
                               after_logout, session_after, error));
  CHECK(error.code == auth_code::unauthorized);

  // Idle expiry: a zero idle window makes the next validation fail.
  session_policy strict;
  strict.idle_ms = 0;
  auth_service expiring(users, sessions, strict);
  login_result expired;
  CHECK(auth.login(*client, dialect, "operator", "another strong password", "",
                   "", expired, error));
  admin_user expired_user;
  session_record expired_session;
  CHECK(!expiring.validate_session(*client, dialect, expired.session_token,
                                   expired_user, expired_session, error));
  CHECK(error.code == auth_code::unauthorized);

  db.release(client);
  db.close();
  std::filesystem::remove_all(dir, ec);

  if (failures != 0) {
    std::fprintf(stderr, "%d auth checks failed\n", failures);
    return 1;
  }
  std::printf("ok: auth service\n");
  return 0;
}
