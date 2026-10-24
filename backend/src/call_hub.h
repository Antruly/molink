#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace molink {

enum class call_phase { idle, ringing, active };

struct call_snapshot {
  call_phase phase = call_phase::idle;
  uint32_t session_id = 0;
  uint64_t started_ms = 0;
  bool host_microphone = false;
};

/// One machine-wide voice call. A remote session invites; the person at the
/// host accepts or declines through /api/call. Only one call runs at a time.
class call_hub {
 public:
  using listener =
      std::function<void(const call_snapshot&, const std::string& event)>;

  call_snapshot snapshot() const;

  bool invite(uint32_t session_id, std::string& err);
  void hangup(uint32_t session_id);
  bool accept(std::string& err);
  bool decline();
  bool end();
  void set_host_microphone(bool available);

  int subscribe(listener fn);
  void unsubscribe(int token);

 private:
  void notify(const std::string& event);

  mutable std::mutex mutex_;
  call_snapshot state_;
  std::vector<std::pair<int, listener>> listeners_;
  int next_token_ = 1;
};

}  // namespace molink
