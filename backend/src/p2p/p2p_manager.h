#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <handle/uvcpp_loop.h>

#include "p2p/ice_agent.h"
#include "server_client.h"
#include "session_controller.h"
#include "session_registry.h"
#include "transport/juice_transport.h"
#include "transport/relay_transport.h"
#include "transport/switchable_transport.h"

namespace molink {

/// Coordinates ICE endpoints for both roles:
///
/// - initiator: `connect()` gathers candidates, creates the signaling session
///   through the directory, feeds the answer back into libjuice and hands the
///   completed transport to the /ws/connect proxy;
/// - target: polls /v1/connect/pending, answers offers, and attaches a
///   session_controller to the completed transport, exactly like the
///   WebSocket handler does.
///
/// ICE and all libuv handles live on the app loop. Signaling HTTP runs on a
/// dedicated worker thread so a slow directory never blocks the loop.
class p2p_manager {
 public:
  using post_fn = std::function<void(std::function<void()>)>;
  using ready_fn = std::function<void(std::shared_ptr<session_transport>)>;
  using error_fn = std::function<void(const std::string&)>;

  p2p_manager(uvcpp::uvcpp_loop* loop, post_fn post,
              std::shared_ptr<server_client> server,
              std::shared_ptr<session_registry> registry,
              session_options options);
  ~p2p_manager();

  p2p_manager(const p2p_manager&) = delete;
  p2p_manager& operator=(const p2p_manager&) = delete;

  void start();
  void stop();

  /// Called on the app loop. `target` is the 9-character device ID.
  void connect(const std::string& target, bool relay_only, ready_fn ready,
               error_fn error);

 private:
  struct outgoing {
    std::shared_ptr<ice_agent> agent;
    std::string target;
    std::string connect_id;
    std::vector<std::string> candidates;
    bool answer_applied = false;
    bool finished = false;
    bool relay_only = false;
    bool delivered = false;
    bool ice_completed = false;
    uint64_t created_ms = 0;
    ready_fn ready;
    error_fn error;
    std::shared_ptr<relay_transport> relay;
    std::shared_ptr<switchable_transport> switcher;
    std::shared_ptr<juice_transport> ice;
  };

  struct incoming {
    std::shared_ptr<ice_agent> agent;
    std::string connect_id;
    std::vector<std::string> candidates;
    bool answered = false;
    bool finished = false;
    bool relay_delivered = false;
    bool ice_delivered = false;
    uint64_t created_ms = 0;
    std::shared_ptr<relay_transport> relay;
    std::shared_ptr<switchable_transport> switcher;
  };

  void worker_loop();
  void enqueue(std::function<void()> task);
  void sweep();
  /// Re-read STUN from settings before a new connection (UI override).
  void refresh_stun();

  std::shared_ptr<ice_agent> make_agent();
  void start_incoming(const signal_session_info& session);
  void handle_status(uint64_t id, const signal_session_info& session);
  void finish_outgoing(uint64_t id, const std::string& error);
  void deliver_outgoing(uint64_t id);
  void finish_incoming(const std::string& connect_id, const std::string& error);
  void deliver_incoming(const std::string& connect_id);
  void start_relay_outgoing(uint64_t id, std::string url, std::string ca);
  void on_outgoing_relay_ready(uint64_t id);
  void start_relay_incoming(const std::string& connect_id, std::string url,
                            std::string ca);
  void on_incoming_relay_ready(const std::string& connect_id);

  static std::pair<std::string, unsigned int> parse_endpoint(
      const std::string& endpoint);

  uvcpp::uvcpp_loop* loop_ = nullptr;
  post_fn post_;
  std::shared_ptr<server_client> server_;
  std::shared_ptr<session_registry> registry_;
  session_options options_;
  std::string stun_host_;
  uint16_t stun_port_ = 3478;

  std::thread worker_;
  std::atomic<bool> running_{false};
  std::mutex task_mutex_;
  std::condition_variable task_cv_;
  std::deque<std::function<void()>> tasks_;

  // Loop-thread state.
  std::map<uint64_t, outgoing> outgoing_;
  std::map<std::string, incoming> incoming_;
  std::set<std::string> completed_incoming_;
  uint64_t next_outgoing_ = 1;

  // Worker <-> loop snapshot of outgoing sessions to poll.
  std::mutex outgoing_mutex_;
  struct outgoing_poll {
    uint64_t id = 0;
    std::string connect_id;
    uint64_t created_ms = 0;
  };
  std::map<uint64_t, outgoing_poll> outgoing_poll_;

  std::mutex seen_mutex_;
  std::set<std::string> seen_incoming_;
};

}  // namespace molink
