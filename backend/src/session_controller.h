#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "access_requests.h"
#include "audio/audio.h"
#include "audio/voice.h"
#include "auth_state.h"
#include "call_hub.h"
#include "capture/capture.h"
#include "capture/stream_qos.h"
#include "chat_hub.h"
#include "clipboard/clipboard.h"
#include "file_transfer.h"
#include "input/input.h"
#include "lock_state.h"
#include "network_settings.h"
#include "session_registry.h"
#include "transport/session_transport.h"

namespace uvcpp {
class uvcpp_web_app;
}

namespace molink {

struct session_options {
  std::shared_ptr<auth_state> auth;
  std::string version = "0.0.0";
  int heartbeat_ms = 15000;
  int hello_timeout_ms = 5000;
  // Background browser tabs throttle timers to about once a minute, so a
  // 45 s idle close dropped otherwise healthy sessions. Five minutes leaves
  // room for a throttled ping while still reaping abandoned tabs.
  int idle_close_ms = 300000;
  std::shared_ptr<capture_source> capture;
  std::shared_ptr<input_injector> input;
  std::shared_ptr<clipboard_service> clipboard;
  std::shared_ptr<lock_state> lock;
  std::shared_ptr<access_request_registry> access_requests;
  std::shared_ptr<network_settings> network;
  std::shared_ptr<chat_hub> chat;
  std::shared_ptr<audio_hub> audio;
  std::shared_ptr<call_hub> call;
  std::shared_ptr<voice_engine> voice;
  std::shared_ptr<std::atomic<int>> bound_port;
  bool http2_available = false;
  /// The QUIC session listener is up and reachable on network->quic_port.
  /// Shared because the listener starts on the app loop *after* the handlers
  /// are registered; the welcome reads the live value.
  std::shared_ptr<std::atomic<bool>> quic_available;
  /// The TCP data listener (`tcp_data_port`) is up.
  std::shared_ptr<std::atomic<bool>> tcp_available;
  /// The HTTP/3 WebSocket listener (`http3_port`) is up.
  std::shared_ptr<std::atomic<bool>> http3_available;
  int tile_size = 0;
  std::string device_id;
  std::string device_name;
  /// Host certificate fingerprint advertised to clients for pinning.
  std::string cert_fingerprint;
  /// SHA-256 of the short-lived WebTransport certificate, when that endpoint
  /// is up. A browser pins it through `serverCertificateHashes`.
  std::string webtransport_fingerprint;
  /// The WebTransport listener is up on network->webtransport_port.
  std::shared_ptr<std::atomic<bool>> webtransport_available;
  /// The host web listener itself speaks TLS (direct LAN encryption).
  bool tls_enabled = false;
  /// Hop a callback onto the app loop (used for clipboard results, which are
  /// produced on the clipboard thread).
  std::function<void(std::function<void()>)> post;
};

/// Transport-independent session state machine: hello, frame pulls, input,
/// clipboard, cursor and the adaptive capture-rate message. A WebSocket
/// adapter feeds it text/binary messages; a future TCP/QUIC adapter can reuse
/// it by implementing session_transport and calling start().
class session_controller final
    : public std::enable_shared_from_this<session_controller> {
 public:
  session_controller(std::shared_ptr<session_registry> registry,
                     std::shared_ptr<session_transport> transport,
                     session_options options);

  /// Registers the session and installs transport handlers. Returns false when
  /// the peer is blocked/banned or its initial token is rejected (the
  /// transport is closed in that case).
  bool start(const std::string* initial_token);

 private:
  void on_text(const std::string& text);
  void on_binary(const uint8_t* data, std::size_t len);
  void on_close(int code, const std::string& reason);
  void send_welcome_message();
  /// Creates the reconnect credential handed out in the welcome. A client
  /// that drops and reconnects presents it instead of spending another use of
  /// a multi-use verification code.
  void issue_resume_token();
  /// Sends the chat history and starts receiving new messages. Called once the
  /// session has authenticated.
  void attach_chat();
  void chat_history();
  void attach_call();
  /// Hop the call event onto the app loop: accept/decline arrive on an HTTP
  /// worker thread, while the voice engine and the websocket are owned by the
  /// app loop. Running both sides on the same thread also keeps the WASAPI
  /// COM objects in one apartment.
  void on_call_event(const call_snapshot& snapshot, const std::string& event);
  void apply_call_event(const call_snapshot& snapshot,
                        const std::string& event);
  void start_voice();
  void stop_voice();
  void send_voice(std::vector<uint8_t> pcm);
  void send_json_string(const std::string& text);
  bool send_file_chunk(uint32_t id, uint64_t offset, const uint8_t* data,
                       std::size_t len);
  void send_audio(std::vector<uint8_t> pcm);
  /// Brings up the WebRTC peer for a session that asked for one (Phase 4).
  void start_rtc(const std::shared_ptr<session_info>& session);
  /// Called from the local approval dialog (through the request registry).
  void approve_access_request();
  void deny_access_request();

  std::shared_ptr<session_registry> registry_;
  std::weak_ptr<session_transport> transport_;
  std::weak_ptr<session_info> session_;
  session_options options_;
  /// Issued once per connection; unknown to the host after a restart, in
  /// which case the client falls back to its verification code.
  std::string resume_token_;
  std::unique_ptr<file_transfer> files_;
  int chat_subscription_ = 0;
  int audio_subscription_ = 0;
  int call_subscription_ = 0;
  bool voice_active_ = false;
  /// Rate limit for reporting silent input-injection failures to the client.
  std::uint64_t last_input_error_ms_ = 0;
};

/// Sends keepalive pings and reaps sessions that never authenticated or went
/// silent. Runs its own thread and posts back to the app loop, because libuv
/// handles are loop-affine.
class session_supervisor {
 public:
  session_supervisor(uvcpp::uvcpp_web_app& app,
                     std::shared_ptr<session_registry> registry,
                     session_options options);
  ~session_supervisor();

  session_supervisor(const session_supervisor&) = delete;
  session_supervisor& operator=(const session_supervisor&) = delete;

  void start();
  void stop();

 private:
  void run();
  /// Fixed frame clock for sessions that asked the host to push video.
  void run_push_clock();

  uvcpp::uvcpp_web_app& app_;
  std::shared_ptr<session_registry> registry_;
  session_options options_;
  std::thread thread_;
  std::thread push_thread_;
  std::atomic<bool> running_{false};
  /// Last AV1 policy value announced to the capture source (diagnostics).
  bool av1_allowed_ = false;
  /// Weight-based stream controller: stretches or squeezes the user's
  /// anchors from the viewers' paint rate, drops and RTT.
  stream_qos qos_;
  /// A viewer joined: the picture starts on the stream instead of tiles so the
  /// first seconds do not pay the full-screen tile keyframe.
  bool had_viewers_ = false;
  static constexpr uint64_t kJoinStreamMs = 10000;
  /// Tiles send changed pixels every frame; above this the link and the
  /// viewer's decompressor are the limit, not the codec.
  static constexpr int kTileMaxFps = 30;
  /// Capture-stats reporting: previous sample, to derive the achieved rate.
  std::chrono::steady_clock::time_point last_capture_at_{};
  uint64_t last_capture_frames_ = 0;
  /// Consecutive one-second samples with a viewer behind; two are needed
  /// before the capture is told to cut detail, so one hiccup cannot soften
  /// the picture.
  int viewer_behind_streak_ = 0;
};

}  // namespace molink
