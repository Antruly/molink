#include "db/db_executor.h"

#include <req/uvcpp_work.h>

#include <memory>

namespace molink {
namespace {

struct job {
  db_executor::body work;
  db_executor::done callback;
  bool transaction = false;
  bool ok = false;
  std::string error;
};

}  // namespace

db_executor::db_executor(database& db) : db_(db) {}

db_executor::~db_executor() { wait_idle(); }

void db_executor::read(uvcpp::uvcpp_loop* loop, body work, done callback) {
  submit(loop, false, std::move(work), std::move(callback));
}

void db_executor::write(uvcpp::uvcpp_loop* loop, body work, done callback) {
  submit(loop, true, std::move(work), std::move(callback));
}

void db_executor::submit(uvcpp::uvcpp_loop* loop, bool transaction, body work,
                         done callback) {
  if (loop == nullptr) {
    if (callback) callback(false, "database work has no event loop");
    return;
  }
  auto state = std::make_shared<job>();
  state->work = std::move(work);
  state->callback = std::move(callback);
  state->transaction = transaction;

  auto* request = new uvcpp::uvcpp_work();
  if (request->init() != 0) {
    delete request;
    if (state->callback) {
      state->callback(false, "could not initialise database work");
    }
    return;
  }
  in_flight_.fetch_add(1);

  database* db = &db_;
  const int queued = request->queue_work(
      loop,
      [db, state](uvcpp::uvcpp_work*) {
        uvcpp::uvcpp_db_client* client = db->acquire(5000);
        if (client == nullptr) {
          state->error = "database pool is exhausted";
          return;
        }
        bool ok = true;
        if (state->transaction) {
          const auto begin = client->begin();
          if (!uvcpp::uvcpp_db_ok(begin)) {
            state->error = "could not begin transaction: " + client->last_error();
            ok = false;
          }
        }
        if (ok && state->work) ok = state->work(*client, state->error);
        if (state->transaction) {
          if (ok) {
            const auto commit = client->commit();
            if (!uvcpp::uvcpp_db_ok(commit)) {
              state->error = "could not commit transaction: " + client->last_error();
              client->rollback();
              ok = false;
            }
          } else {
            client->rollback();
          }
        }
        state->ok = ok;
        // The pool pings on acquire; a connection that broke mid-query is
        // discarded by the next borrower. Release is enough here.
        db->release(client);
      },
      [this, state, request](uvcpp::uvcpp_work*, int status) {
        if (status != 0 && state->ok) {
          state->ok = false;
          state->error = "database work was cancelled";
        }
        if (state->callback) state->callback(state->ok, state->error);
        finish();
        delete request;
      });
  if (queued != 0) {
    finish();
    delete request;
    if (state->callback) {
      state->callback(false, "could not queue database work");
    }
  }
}

void db_executor::finish() {
  if (in_flight_.fetch_sub(1) == 1) {
    std::lock_guard<std::mutex> lock(mutex_);
    idle_.notify_all();
  }
}

std::size_t db_executor::in_flight() const { return in_flight_.load(); }

void db_executor::wait_idle() {
  std::unique_lock<std::mutex> lock(mutex_);
  idle_.wait(lock, [this] { return in_flight_.load() == 0; });
}

}  // namespace molink
