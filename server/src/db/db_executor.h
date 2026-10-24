#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>

#include <db/uvcpp_db.h>
#include <handle/uvcpp_loop.h>

#include "db/database.h"

namespace molink {

/// Runs repository work on the libuv worker pool and delivers the result on
/// the calling event loop. Handlers must never call the synchronous
/// repositories directly.
class db_executor {
 public:
  using body = std::function<bool(uvcpp::uvcpp_db_client&, std::string&)>;
  using done = std::function<void(bool ok, const std::string& error)>;

  explicit db_executor(database& db);
  ~db_executor();

  db_executor(const db_executor&) = delete;
  db_executor& operator=(const db_executor&) = delete;

  /// Borrow a connection, run `work`, release it.
  void read(uvcpp::uvcpp_loop* loop, body work, done callback);
  /// Same, inside BEGIN/COMMIT with rollback on any failure.
  void write(uvcpp::uvcpp_loop* loop, body work, done callback);

  std::size_t in_flight() const;
  /// Block until every queued operation has completed (shutdown only).
  void wait_idle();

 private:
  void submit(uvcpp::uvcpp_loop* loop, bool transaction, body work,
              done callback);
  void finish();

  database& db_;
  std::atomic<std::size_t> in_flight_{0};
  std::mutex mutex_;
  std::condition_variable idle_;
};

}  // namespace molink
