#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace molink {

/// One message in the session chat. `from` is "remote" for a controlling
/// client and "host" for the person sitting at the machine.
struct chat_message {
  uint64_t id = 0;
  std::string from;
  /// Who typed it: the remote machine's name/id, or the host's. A browser
  /// client without a device identity is labelled by its peer address.
  std::string sender_name;
  std::string sender_id;
  /// Host-side session that posted it (0 for the person at the machine).
  /// Clients compare it with their welcome.session_id to mark their own
  /// messages, which matters when several viewers share one chat.
  uint32_t sender_session = 0;
  std::string text;
  uint64_t ts_ms = 0;
};

/// A small, shared chat log. Every authenticated session subscribes so a
/// message from the remote viewer also shows up in the local UI (which polls
/// /api/chat) and in any other viewer.
class chat_hub {
 public:
  using listener = std::function<void(const chat_message&)>;

  uint64_t post(const std::string& from, const std::string& text,
                const std::string& sender_name, const std::string& sender_id,
                uint32_t sender_session);
  std::vector<chat_message> history(uint64_t since) const;
  uint64_t cursor() const;

  /// Returns a subscription token used to unsubscribe when the session closes.
  int subscribe(listener fn);
  void unsubscribe(int token);

 private:
  mutable std::mutex mutex_;
  std::deque<chat_message> messages_;
  struct subscriber {
    int token = 0;
    listener fn;
  };
  std::vector<subscriber> subscribers_;
  uint64_t next_id_ = 1;
  int next_token_ = 1;
};

}  // namespace molink
