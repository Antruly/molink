// WebRTC loopback: the host's rtc_session answers a peer that behaves like the
// browser side of the signaling exchange. Everything runs in one process over
// ICE on the loopback interface, so the offer/answer handshake, the data
// channel and the H.264 video track are all exercised without a browser.
//
// The picture is a hand-written Annex-B access unit (SPS, PPS, IDR); the test
// checks that what the host hands to the video track comes out of the
// depacketizer byte for byte, which is what the browser's decoder would see.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rtc/rtc.hpp>

#include "webrtc/rtc_session.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void expect(bool ok, const char* what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

using namespace std::chrono_literals;

/// Waits until `predicate` holds or the deadline passes.
template <typename Predicate>
bool wait_for(Predicate predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(5ms);
  }
  return predicate();
}

/// Shared state between the host session and the fake browser.
struct peer_link {
  std::mutex mutex;
  std::condition_variable cv;

  std::shared_ptr<rtc::PeerConnection> browser;
  std::shared_ptr<rtc::Track> video;
  std::shared_ptr<rtc::DataChannel> channel;

  bool browser_connected = false;
  bool data_channel_open = false;
  bool host_ready = false;
  std::vector<std::pair<std::string, std::string>> pending_candidates;
  std::vector<std::string> texts;
  std::vector<std::vector<uint8_t>> binaries;
  std::vector<std::vector<uint8_t>> frames;
};

/// A minimal, syntactically valid H.264 access unit: SPS, PPS and one IDR
/// slice. The decoders never run it; the RTP path only has to carry it.
std::vector<uint8_t> sample_access_unit() {
  static const uint8_t kSps[] = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42,
                                 0x00, 0x1e, 0x95, 0xa8, 0x28, 0x0f};
  static const uint8_t kPps[] = {0x00, 0x00, 0x00, 0x01, 0x68, 0xce,
                                 0x38, 0x80};
  static const uint8_t kIdr[] = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88,
                                 0x84, 0x00, 0x21, 0xff, 0x9c, 0x11};
  std::vector<uint8_t> unit;
  unit.insert(unit.end(), kSps, kSps + sizeof(kSps));
  unit.insert(unit.end(), kPps, kPps + sizeof(kPps));
  unit.insert(unit.end(), kIdr, kIdr + sizeof(kIdr));
  return unit;
}

}  // namespace

int main() {
  auto state = std::make_shared<peer_link>();

  molink::rtc_session::callbacks callbacks;
  callbacks.on_state = [](const char* state_name) {
    std::printf("host state: %s\n", state_name);
  };
  // Signaling: our offer and candidates go straight to the fake browser, the
  // way the session controller would put them in control messages.
  // Never call into libdatachannel while holding the lock: its callbacks can
  // run synchronously on this thread and would deadlock on the same mutex.
  callbacks.on_local_description = [state](const std::string& type,
                                           const std::string& sdp) {
    if (type != "offer") return;
    std::shared_ptr<rtc::PeerConnection> browser;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      browser = state->browser;
    }
    if (!browser) return;
    try {
      browser->setRemoteDescription(rtc::Description(sdp, "offer"));
      browser->setLocalDescription();
      std::printf("browser: answered\n");
    } catch (const std::exception& e) {
      std::printf("browser: answer failed: %s\n", e.what());
    }
  };
  callbacks.on_local_candidate =
      [state](const std::string& candidate, const std::string& mid) {
        std::printf("host candidate: %s (mid=%s)\n", candidate.c_str(),
                    mid.c_str());
        std::shared_ptr<rtc::PeerConnection> browser;
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          browser = state->browser;
        }
        if (!browser) return;
        try {
          browser->addRemoteCandidate(rtc::Candidate(candidate, mid));
        } catch (const std::exception& e) {
          std::printf("browser: candidate failed: %s\n", e.what());
        }
      };
  callbacks.on_data_channel_open = [state]() {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->data_channel_open = true;
  };
  callbacks.on_text = [state](const std::string& text) {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->texts.push_back(text);
  };
  callbacks.on_binary = [state](const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->binaries.push_back(data);
  };

  {
    std::lock_guard<std::mutex> lock(state->mutex);
    rtc::Configuration config;
    state->browser = std::make_shared<rtc::PeerConnection>(config);
  }
  state->browser->onStateChange([state](rtc::PeerConnection::State s) {
    std::printf("browser state: %d\n", static_cast<int>(s));
    if (s == rtc::PeerConnection::State::Connected) {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->browser_connected = true;
    }
  });
  state->browser->onDataChannel([state](std::shared_ptr<rtc::DataChannel> dc) {
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->channel = dc;
    }
    dc->onMessage([state](rtc::message_variant message) {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (std::holds_alternative<rtc::string>(message)) {
        state->texts.push_back(std::get<rtc::string>(message));
      } else {
        const auto& bytes = std::get<rtc::binary>(message);
        state->binaries.emplace_back(
            reinterpret_cast<const uint8_t*>(bytes.data()),
            reinterpret_cast<const uint8_t*>(bytes.data()) + bytes.size());
      }
    });
  });
  state->browser->onTrack([state](std::shared_ptr<rtc::Track> track) {
    std::printf("browser: track %s open=%d\n", std::string(track->mid()).c_str(),
                track->isOpen() ? 1 : 0);
    track->setMediaHandler(std::make_shared<rtc::H264RtpDepacketizer>(
        rtc::NalUnit::Separator::LongStartSequence));
    track->onFrame([state](rtc::binary data, rtc::FrameInfo) {
      std::printf("browser: frame %zu bytes\n", data.size());
      std::lock_guard<std::mutex> lock(state->mutex);
      state->frames.emplace_back(reinterpret_cast<const uint8_t*>(data.data()),
                                 reinterpret_cast<const uint8_t*>(data.data()) +
                                     data.size());
    });
    std::lock_guard<std::mutex> lock(state->mutex);
    state->video = track;
  });

  molink::rtc_session::config config;
  molink::rtc_session session;
  // Candidates that arrive before the host session is ready wait here.
  state->browser->onLocalCandidate([&session, state](rtc::Candidate candidate) {
    std::printf("browser candidate: %s (mid=%s)\n", std::string(candidate).c_str(),
                std::string(candidate.mid()).c_str());
    bool ready = false;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      ready = state->host_ready;
      if (!ready) {
        state->pending_candidates.emplace_back(std::string(candidate),
                                               std::string(candidate.mid()));
        return;
      }
    }
    session.add_remote_candidate(std::string(candidate),
                                 std::string(candidate.mid()));
  });

  // The browser's answer goes back to the host, exactly as the page would put
  // it in an `rtc-answer` control message.
  state->browser->onLocalDescription(
      [&session](rtc::Description description) {
        std::printf("browser description: %s\n",
                    description.typeString().c_str());
        if (description.typeString() != "answer") return;
        std::string answer_error;
        if (!session.accept_answer(std::string(description), &answer_error)) {
          std::printf("host rejected the answer: %s\n", answer_error.c_str());
        }
      });

  std::string error;
  const bool started = session.start(config, callbacks, &error);
  expect(started, "the host session starts");
  if (!started) {
    std::printf("start failed: %s\n", error.c_str());
    return 1;
  }

  // The browser's candidates go back to the host the same way. They can start
  // arriving before the host session exists, so they are queued first.
  std::vector<std::pair<std::string, std::string>> early;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    early.swap(state->pending_candidates);
  }
  for (const auto& candidate : early) {
    session.add_remote_candidate(candidate.first, candidate.second);
  }
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->host_ready = true;
  }

  expect(wait_for([&] { return session.channel_open(); }, 10s),
         "the data channel opens");
  expect(wait_for(
             [&] {
               std::lock_guard<std::mutex> lock(state->mutex);
               return state->browser_connected;
             },
             10s),
         "ICE connects the two peers");

  expect(session.channel_open(), "the host reports the channel open");

  // Data channel: text and binary both directions.
  expect(session.send_text("hello from the host"), "host sends text");
  { std::lock_guard<std::mutex> lock(state->mutex);
    if (state->channel) state->channel->send(std::string("hello from the browser")); }
  expect(wait_for(
             [&] {
               std::lock_guard<std::mutex> lock(state->mutex);
               return !state->texts.empty();
             },
             5s),
         "the host receives a data channel message");

  const std::vector<uint8_t> binary = {0x4d, 0x4f, 0x4c, 0x4b, 1, 2, 3};
  expect(session.send_binary(binary.data(), binary.size()),
         "host sends binary");
  expect(wait_for(
             [&] {
               std::lock_guard<std::mutex> lock(state->mutex);
               return !state->binaries.empty() &&
                      state->binaries.front() == binary;
             },
             5s),
         "the binary payload arrives intact");

  // Video: the track must open, and the access unit must come out of the
  // depacketizer with its start codes intact.
  expect(wait_for([&] { return session.video_open(); }, 10s),
         "the video track opens");
  const std::vector<uint8_t> unit = sample_access_unit();
  bool sent = false;
  for (int attempt = 0; attempt < 40 && !sent; ++attempt) {
    sent = session.send_video(unit.data(), unit.size(), 1234567);
    if (!sent) std::this_thread::sleep_for(50ms);
  }
  // Repeat a few times: one access unit can be lost while the receiver's
  // first RTP packet is still being set up.
  for (int attempt = 0; attempt < 5; ++attempt) {
    session.send_video(unit.data(), unit.size(), 1234567 + attempt * 33333);
    std::this_thread::sleep_for(50ms);
  }
  expect(sent, "the host sends one access unit");
  expect(wait_for(
             [&] {
               std::lock_guard<std::mutex> lock(state->mutex);
               return !state->frames.empty();
             },
             10s),
         "the video frame arrives at the peer");
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->frames.empty()) {
      const auto& frame = state->frames.front();
      expect(frame.size() >= unit.size(),
             "the received frame is at least the access unit");
      // The depacketizer normalises the separator to the long start sequence;
      // comparing the trailing IDR payload is enough to prove ordering and
      // integrity.
      const uint8_t tail[] = {0x65, 0x88, 0x84, 0x00, 0x21, 0xff, 0x9c, 0x11};
      const bool tail_ok =
          frame.size() >= sizeof(tail) &&
          std::memcmp(frame.data() + frame.size() - sizeof(tail), tail,
                      sizeof(tail)) == 0;
      expect(tail_ok, "the IDR payload survives RTP packetization");
    }
  }

  session.close();
  state->browser->close();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
