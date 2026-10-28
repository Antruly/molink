#include "webrtc/rtc_session.h"

#include <memory>
#include <utility>

#include <rtc/rtc.hpp>

namespace molink {

struct rtc_session::impl {
  rtc_session::config config;
  rtc_session::callbacks callbacks;
  std::shared_ptr<rtc::PeerConnection> peer;
  std::shared_ptr<rtc::Track> video;
  std::shared_ptr<rtc::DataChannel> channel;
  std::shared_ptr<rtc::DataChannel> audio;
  std::shared_ptr<rtc::DataChannel> file;
  bool audio_open = false;
  bool file_open = false;
  std::shared_ptr<rtc::RtpPacketizationConfig> video_config;
  bool channel_open = false;
  uint32_t last_timestamp = 0;

  void emit_state(const char* state) {
    if (callbacks.on_state) callbacks.on_state(state);
  }
};

rtc_session::rtc_session() : impl_(std::make_shared<impl>()) {}

rtc_session::~rtc_session() { close(); }

bool rtc_session::start(const config& cfg, callbacks cb, std::string* error) {
  if (error != nullptr) error->clear();
  if (impl_ == nullptr) return false;
  impl_->config = cfg;
  impl_->callbacks = std::move(cb);

  rtc::Configuration rtc_config;
  for (const std::string& url : cfg.ice_servers) {
    try {
      rtc_config.iceServers.emplace_back(url);
    } catch (const std::exception& e) {
      if (error != nullptr) {
        *error = std::string("rtc: bad ICE server '") + url + "': " + e.what();
      }
      return false;
    }
  }
  // A screen share is latency-sensitive and mostly LAN/TURN relay traffic;
  // the default MTU is fine, and a bounded message size keeps one oversized
  // file chunk from tearing the channel down.
  rtc_config.maxMessageSize = 1024 * 1024;

  try {
    impl_->peer = std::make_shared<rtc::PeerConnection>(rtc_config);
  } catch (const std::exception& e) {
    if (error != nullptr) *error = std::string("rtc: ") + e.what();
    return false;
  }

  std::weak_ptr<impl> weak = impl_;
  impl_->peer->onStateChange([weak](rtc::PeerConnection::State state) {
    const auto self = weak.lock();
    if (!self) return;
    switch (state) {
      case rtc::PeerConnection::State::New:
        self->emit_state("new");
        break;
      case rtc::PeerConnection::State::Connecting:
        self->emit_state("connecting");
        break;
      case rtc::PeerConnection::State::Connected:
        self->emit_state("connected");
        break;
      case rtc::PeerConnection::State::Disconnected:
        self->emit_state("disconnected");
        break;
      case rtc::PeerConnection::State::Failed:
        self->emit_state("failed");
        break;
      case rtc::PeerConnection::State::Closed:
        self->emit_state("closed");
        break;
    }
  });
  impl_->peer->onLocalDescription(
      [weak](rtc::Description description) {
        const auto self = weak.lock();
        if (!self || !self->callbacks.on_local_description) return;
        self->callbacks.on_local_description(
            description.typeString(), std::string(description));
      });
  impl_->peer->onLocalCandidate([weak](rtc::Candidate candidate) {
    const auto self = weak.lock();
    if (!self || !self->callbacks.on_local_candidate) return;
    self->callbacks.on_local_candidate(std::string(candidate),
                                       std::string(candidate.mid()));
  });

  try {
    // Video: one send-only H.264 track. The payload type and SSRC are declared
    // here, so the browser's answer maps its decoder onto the same values.
    rtc::Description::Video media("video",
                                  rtc::Description::Direction::SendOnly);
    media.addH264Codec(cfg.video_payload_type);
    media.addSSRC(cfg.video_ssrc, "molink-video");
    impl_->video = impl_->peer->addTrack(media);
    auto rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(
        cfg.video_ssrc, "molink-video",
        static_cast<uint8_t>(cfg.video_payload_type),
        rtc::H264RtpPacketizer::defaultClockRate);
    auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
        rtc::NalUnit::Separator::StartSequence, rtp_config);
    impl_->video->setMediaHandler(packetizer);
    // A sender that ignores PLI/FIR leaves the browser frozen after a loss:
    // chain the responder so a keyframe request reaches the capture.
    impl_->video->chainMediaHandler(std::make_shared<rtc::PliHandler>([weak]() {
      const auto self = weak.lock();
      if (self && self->callbacks.on_keyframe_request) {
        self->callbacks.on_keyframe_request();
      }
    }));
    // The packetizer owns the timestamp; keep the config so send_video can
    // advance it. A shared_ptr keeps it alive past this scope.
    impl_->video_config = rtp_config;
    impl_->video->onOpen([weak]() {
      const auto self = weak.lock();
      if (self) self->emit_state("video-open");
    });
    impl_->video->onClosed([weak]() {
      const auto self = weak.lock();
      if (self) self->emit_state("video-closed");
    });

    impl_->channel = impl_->peer->createDataChannel(cfg.data_channel_label);
    impl_->channel->onOpen([weak]() {
      const auto self = weak.lock();
      if (!self) return;
      self->channel_open = true;
      self->emit_state("channel-open");
      if (self->callbacks.on_data_channel_open) self->callbacks.on_data_channel_open();
    });
    impl_->channel->onClosed([weak]() {
      const auto self = weak.lock();
      if (!self) return;
      self->channel_open = false;
      self->emit_state("channel-closed");
    });
    rtc::DataChannelInit audio_init;
    audio_init.reliability.unordered = true;
    audio_init.reliability.maxRetransmits = 0;
    impl_->audio = impl_->peer->createDataChannel(cfg.audio_channel_label,
                                                  audio_init);
    impl_->audio->onOpen([weak]() {
      const auto self = weak.lock();
      if (!self) return;
      self->audio_open = true;
      self->emit_state("audio-open");
    });
    impl_->audio->onClosed([weak]() {
      const auto self = weak.lock();
      if (!self) return;
      self->audio_open = false;
      self->emit_state("audio-closed");
    });

    // File traffic gets its own reliable, ordered channel: a transfer then
    // cannot occupy the control channel's send queue.
    impl_->file = impl_->peer->createDataChannel(cfg.file_channel_label);
    impl_->file->onOpen([weak]() {
      const auto self = weak.lock();
      if (!self) return;
      self->file_open = true;
      self->emit_state("file-open");
    });
    impl_->file->onClosed([weak]() {
      const auto self = weak.lock();
      if (!self) return;
      self->file_open = false;
      self->emit_state("file-closed");
    });

    impl_->channel->onMessage([weak](rtc::message_variant message) {
      const auto self = weak.lock();
      if (!self) return;
      if (std::holds_alternative<rtc::string>(message)) {
        if (self->callbacks.on_text) {
          self->callbacks.on_text(std::get<rtc::string>(message));
        }
        return;
      }
      const auto& bytes = std::get<rtc::binary>(message);
      if (self->callbacks.on_binary) {
        std::vector<uint8_t> copy(
            reinterpret_cast<const uint8_t*>(bytes.data()),
            reinterpret_cast<const uint8_t*>(bytes.data()) + bytes.size());
        self->callbacks.on_binary(copy);
      }
    });
  } catch (const std::exception& e) {
    if (error != nullptr) *error = std::string("rtc: ") + e.what();
    return false;
  }

  impl_->peer->setLocalDescription();
  return true;
}

bool rtc_session::accept_answer(const std::string& sdp, std::string* error) {
  if (error != nullptr) error->clear();
  if (impl_ == nullptr || !impl_->peer) {
    if (error != nullptr) *error = "rtc: no peer";
    return false;
  }
  try {
    impl_->peer->setRemoteDescription(rtc::Description(sdp, "answer"));
    return true;
  } catch (const std::exception& e) {
    if (error != nullptr) *error = std::string("rtc: ") + e.what();
    return false;
  }
}

void rtc_session::add_remote_candidate(const std::string& candidate,
                                       const std::string& mid) {
  if (impl_ == nullptr || !impl_->peer) return;
  try {
    impl_->peer->addRemoteCandidate(rtc::Candidate(candidate, mid));
  } catch (...) {
    // A malformed candidate is the browser's problem: dropping it costs one
    // path, not the session -- ICE has others.
  }
}

bool rtc_session::channel_open() const {
  return impl_ != nullptr && impl_->channel_open;
}

bool rtc_session::video_open() const {
  return impl_ != nullptr && impl_->video && impl_->video->isOpen();
}

bool rtc_session::send_text(const std::string& text) {
  if (!channel_open()) return false;
  try {
    return impl_->channel->send(text);
  } catch (...) {
    return false;
  }
}

bool rtc_session::send_audio(const uint8_t* data, std::size_t len) {
  if (!audio_open() || data == nullptr || len == 0) return false;
  try {
    return impl_->audio->send(reinterpret_cast<const std::byte*>(data), len);
  } catch (...) {
    return false;
  }
}

bool rtc_session::send_file(const uint8_t* data, std::size_t len) {
  if (!file_open() || data == nullptr || len == 0) return false;
  try {
    return impl_->file->send(reinterpret_cast<const std::byte*>(data), len);
  } catch (...) {
    return false;
  }
}

bool rtc_session::file_open() const {
  return impl_ != nullptr && impl_->file_open;
}

bool rtc_session::audio_open() const {
  return impl_ != nullptr && impl_->audio_open;
}

bool rtc_session::send_binary(const uint8_t* data, std::size_t len) {
  if (!channel_open() || data == nullptr) return false;
  try {
    return impl_->channel->send(
        reinterpret_cast<const std::byte*>(data), len);
  } catch (...) {
    return false;
  }
}

bool rtc_session::send_video(const uint8_t* annexb, std::size_t len,
                             uint64_t ts_us) {
  if (!video_open() || annexb == nullptr || len == 0) return false;
  if (impl_->video_config) {
    // The RTP clock is 90 kHz, and the browser uses the timestamp deltas for
    // its jitter buffer; a monotonic wall-clock mapping is enough.
    const uint32_t stamp =
        static_cast<uint32_t>((ts_us * 90ull) / 1000ull);
    if (stamp > impl_->last_timestamp) {
      impl_->video_config->timestamp = stamp;
      impl_->last_timestamp = stamp;
    }
  }
  try {
    impl_->video->send(reinterpret_cast<const std::byte*>(annexb), len);
    return true;
  } catch (...) {
    return false;
  }
}

void rtc_session::close() {
  if (impl_ == nullptr) return;
  impl_->channel_open = false;
  impl_->audio_open = false;
  impl_->file_open = false;
  try {
    if (impl_->channel) impl_->channel->close();
  } catch (...) {
  }
  try {
    if (impl_->audio) impl_->audio->close();
  } catch (...) {
  }
  try {
    if (impl_->file) impl_->file->close();
  } catch (...) {
  }
  try {
    if (impl_->peer) impl_->peer->close();
  } catch (...) {
  }
  impl_->channel.reset();
  impl_->audio.reset();
  impl_->file.reset();
  impl_->video.reset();
  impl_->peer.reset();
}

}  // namespace molink
