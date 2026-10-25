#include "chat_hub.h"

#include <chrono>
#include <utility>

namespace molink {
namespace {

constexpr std::size_t kMaxMessages = 200;

uint64_t epoch_ms() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch())
          .count());
}

}  // namespace

uint64_t chat_hub::post(const std::string& from, const std::string& text,
                        const std::string& sender_name,
                        const std::string& sender_id,
                        uint32_t sender_session) {
  std::vector<subscriber> subscribers;
  chat_message message;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    message.id = next_id_++;
    message.from = from;
    message.sender_name = sender_name;
    message.sender_id = sender_id;
    message.sender_session = sender_session;
    message.text = text;
    message.ts_ms = epoch_ms();
    messages_.push_back(message);
    while (messages_.size() > kMaxMessages) messages_.pop_front();
    subscribers = subscribers_;
  }
  // Deliver outside the lock: a subscriber may send on the socket or hop back
  // onto the app loop, and neither should block another post.
  for (const subscriber& item : subscribers) item.fn(message);
  return message.id;
}

std::vector<chat_message> chat_hub::history(uint64_t since) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<chat_message> out;
  out.reserve(messages_.size());
  for (const chat_message& message : messages_) {
    if (message.id > since) out.push_back(message);
  }
  return out;
}

uint64_t chat_hub::cursor() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return messages_.empty() ? 0 : messages_.back().id;
}

int chat_hub::subscribe(listener fn) {
  std::lock_guard<std::mutex> lock(mutex_);
  const int token = next_token_++;
  subscribers_.push_back({token, std::move(fn)});
  return token;
}

void chat_hub::unsubscribe(int token) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = subscribers_.begin(); it != subscribers_.end(); ++it) {
    if (it->token == token) {
      subscribers_.erase(it);
      return;
    }
  }
}

}  // namespace molink
