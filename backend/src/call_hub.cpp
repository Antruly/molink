#include "call_hub.h"

#include <chrono>

namespace {

uint64_t epoch_ms() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch())
          .count());
}

}  // namespace

namespace molink {

call_snapshot call_hub::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

bool call_hub::invite(uint32_t session_id, std::string& err) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.phase != call_phase::idle) {
      if (state_.session_id == session_id) return true;
      err = "another call is already in progress";
      return false;
    }
    state_.phase = call_phase::ringing;
    state_.session_id = session_id;
    state_.started_ms = epoch_ms();
  }
  notify("ringing");
  return true;
}

void call_hub::hangup(uint32_t session_id) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.phase == call_phase::idle || state_.session_id != session_id) {
      return;
    }
    state_.phase = call_phase::idle;
    state_.session_id = 0;
    state_.started_ms = 0;
  }
  notify("ended");
}

bool call_hub::accept(std::string& err) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.phase != call_phase::ringing) {
      err = "no call is ringing";
      return false;
    }
    state_.phase = call_phase::active;
    state_.started_ms = epoch_ms();
  }
  notify("accepted");
  return true;
}

bool call_hub::decline() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.phase != call_phase::ringing) return false;
    state_.phase = call_phase::idle;
    state_.session_id = 0;
    state_.started_ms = 0;
  }
  notify("declined");
  return true;
}

bool call_hub::end() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.phase == call_phase::idle) return false;
    state_.phase = call_phase::idle;
    state_.session_id = 0;
    state_.started_ms = 0;
  }
  notify("ended");
  return true;
}

void call_hub::set_host_microphone(bool available) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.host_microphone = available;
}

int call_hub::subscribe(listener fn) {
  std::lock_guard<std::mutex> lock(mutex_);
  const int token = next_token_++;
  listeners_.emplace_back(token, std::move(fn));
  return token;
}

void call_hub::unsubscribe(int token) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
    if (it->first == token) {
      listeners_.erase(it);
      return;
    }
  }
}

void call_hub::notify(const std::string& event) {
  std::vector<listener> listeners;
  call_snapshot state;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    state = state_;
    for (const auto& item : listeners_) listeners.push_back(item.second);
  }
  for (const listener& fn : listeners) {
    if (fn) fn(state, event);
  }
}

}  // namespace molink
