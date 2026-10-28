#include "transport/channel_scheduler.h"

#include <algorithm>

namespace molink {
namespace {

bool is_video(transport_channel channel) {
  return channel == transport_channel::video;
}

}  // namespace

channel_scheduler::channel_scheduler() : channel_scheduler(limits()) {}

channel_scheduler::channel_scheduler(limits configured) {
  control_.limit = configured.control_bytes;
  video_.limit = configured.video_bytes;
  audio_.limit = configured.audio_bytes;
  file_.limit = configured.file_bytes;
}

channel_scheduler::queue& channel_scheduler::queue_for(
    transport_channel channel) {
  switch (channel) {
    case transport_channel::control:
      return control_;
    case transport_channel::video:
      return video_;
    case transport_channel::audio:
      return audio_;
    case transport_channel::file:
      return file_;
  }
  return control_;
}

const channel_scheduler::queue& channel_scheduler::queue_for(
    transport_channel channel) const {
  switch (channel) {
    case transport_channel::control:
      return control_;
    case transport_channel::video:
      return video_;
    case transport_channel::audio:
      return audio_;
    case transport_channel::file:
      return file_;
  }
  return control_;
}

bool channel_scheduler::evict_video_for(std::size_t required,
                                        bool incoming_keyframe) {
  // A newer keyframe is a complete decoder refresh point, so all queued video
  // before it (including older keyframes and dependent deltas) is obsolete.
  if (incoming_keyframe) {
    video_.messages.clear();
    video_.bytes = 0;
  }

  // One frame always has to fit, or a single uncompressed keyframe larger than
  // the whole video budget could never be sent at all. Holding it delays newer
  // deltas, which could not be decoded before it anyway.
  if (video_.messages.empty()) return true;

  while (true) {
    // A held oversized frame leaves no room at all, and the subtraction must
    // not wrap.
    const std::size_t available =
        video_.bytes >= video_.limit ? 0 : video_.limit - video_.bytes;
    if (required <= available) return true;
    auto candidate = video_.messages.end();
    for (auto it = video_.messages.begin(); it != video_.messages.end(); ++it) {
      if (!it->keyframe) {
        candidate = it;
        break;
      }
    }
    if (candidate == video_.messages.end()) return false;
    video_.bytes -= candidate->data.size();
    video_.messages.erase(candidate);
  }
}

bool channel_scheduler::enqueue(transport_channel channel, const uint8_t* data,
                                std::size_t len, bool keyframe) {
  if (data == nullptr && len != 0) return false;
  queue& target = queue_for(channel);
  if (!is_video(channel)) {
    if (len > target.limit) return false;
    if (len > target.limit - target.bytes) return false;
  } else if (!evict_video_for(len, keyframe)) {
    return false;
  }

  message item;
  item.channel = channel;
  item.keyframe = keyframe;
  if (len != 0) item.data.assign(data, data + len);
  target.bytes += len;
  target.messages.push_back(std::move(item));
  return true;
}

bool channel_scheduler::enqueue_text(const std::string& text) {
  const auto* data = reinterpret_cast<const uint8_t*>(text.data());
  if (text.size() > control_.limit - control_.bytes) {
    return false;
  }
  message item;
  item.channel = transport_channel::control;
  item.text = true;
  item.data.assign(data, data + text.size());
  control_.bytes += item.data.size();
  control_.messages.push_back(std::move(item));
  return true;
}

std::size_t channel_scheduler::drain(
    const std::function<bool(const message&)>& send, std::size_t max_messages) {
  if (!send || max_messages == 0) return 0;
  std::size_t sent = 0;
  bool blocked[4] = {false, false, false, false};
  // Control and audio are latency-sensitive; file is deliberately last.
  const transport_channel order[] = {transport_channel::control,
                                     transport_channel::audio,
                                     transport_channel::video,
                                     transport_channel::file};
  while (sent < max_messages) {
    bool progressed = false;
    for (std::size_t index = 0; index < 4; ++index) {
      const transport_channel channel = order[index];
      queue& source = queue_for(channel);
      if (blocked[index] || source.messages.empty()) continue;
      if (!send(source.messages.front())) {
        // Backpressure is scoped to this channel. Preserve its head message,
        // but allow independent channels to make progress during this drain.
        blocked[index] = true;
        continue;
      }
      source.bytes -= source.messages.front().data.size();
      source.messages.pop_front();
      ++sent;
      progressed = true;
      if (sent == max_messages) break;
    }
    if (!progressed) break;
  }
  return sent;
}

std::size_t channel_scheduler::queued_bytes(transport_channel channel) const {
  return queue_for(channel).bytes;
}

std::size_t channel_scheduler::queued_messages(transport_channel channel) const {
  return queue_for(channel).messages.size();
}

bool channel_scheduler::empty() const {
  return control_.messages.empty() && video_.messages.empty() &&
         audio_.messages.empty() && file_.messages.empty();
}

}  // namespace molink
