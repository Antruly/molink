#include "audio/audio.h"

#include <utility>

namespace molink {

audio_hub::audio_hub(std::unique_ptr<audio_source> source)
    : source_(std::move(source)) {}

audio_hub::~audio_hub() {
  bool stop = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    listeners_.clear();
    stop = running_;
    running_ = false;
  }
  if (stop && source_) source_->stop();
}

bool audio_hub::available() const {
  return source_ != nullptr && source_->available();
}

audio_format audio_hub::format() const {
  return source_ ? source_->format() : audio_format{};
}

int audio_hub::subscribe(listener fn, std::string& err) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!source_ || !source_->available()) {
    err = "audio capture is not available";
    return 0;
  }
  const int token = next_token_++;
  listeners_.emplace_back(token, std::move(fn));
  if (!running_) {
    std::string start_error;
    if (!source_->start(
            [this](std::vector<uint8_t> pcm) { dispatch(std::move(pcm)); },
            start_error)) {
      listeners_.pop_back();
      err = start_error.empty() ? "could not start audio capture" : start_error;
      return 0;
    }
    running_ = true;
  }
  return token;
}

void audio_hub::unsubscribe(int token) {
  bool stop = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
      if (it->first == token) {
        listeners_.erase(it);
        break;
      }
    }
    if (listeners_.empty() && running_) {
      running_ = false;
      stop = true;
    }
  }
  // stop() joins the capture thread; do it without the mutex because the
  // capture thread may still be delivering a callback.
  if (stop && source_) source_->stop();
}

void audio_hub::dispatch(std::vector<uint8_t> pcm) {
  std::vector<listener> listeners;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || listeners_.empty()) return;
    listeners.reserve(listeners_.size());
    for (const auto& item : listeners_) listeners.push_back(item.second);
  }
  for (const listener& fn : listeners) {
    if (fn) fn(pcm);
  }
}

}  // namespace molink
