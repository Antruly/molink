#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace molink {

/// One WebRTC peer for a session (Phase 4).
///
/// The host is always the offerer: it adds a send-only H.264 video track that
/// the browser decodes natively, plus one reliable ordered data channel that
/// carries the same MOLK bytes the WebSocket does. Signaling travels over the
/// existing session socket -- the host sends its offer in a `rtc-offer`
/// control message and receives the answer as `rtc-answer` -- so a client that
/// never answers simply stays on the WebSocket path.
///
/// H.264 is the only video codec on the track on purpose: every browser
/// decodes it, and the host already has a hardware encoder for it (NVENC). AV1
/// stays on the MOLK/WebSocket path for clients that negotiated it.
///
/// Built only when MOLINK_ENABLE_WEBRTC is on (needs libdatachannel).
class rtc_session {
 public:
  struct config {
    /// ICE servers as "stun:host:port" / "turn:host:port?transport=udp".
    /// Empty means host candidates only (fine on a LAN).
    std::vector<std::string> ice_servers;
    /// Payload type and SSRC for the H.264 track. The offer declares them, so
    /// the browser answers with the same values.
    int video_payload_type = 96;
    uint32_t video_ssrc = 42;
    /// Data channel label; the browser sees it in `ondatachannel`.
    std::string data_channel_label = "molink";
    /// Audio channel label. This one is unreliable and unordered: a late
    /// audio packet is worth less than a queued one.
    std::string audio_channel_label = "audio";
  };

  struct callbacks {
    /// Our offer or answer, ready to be sent as a control message.
    std::function<void(const std::string& type, const std::string& sdp)>
        on_local_description;
    /// One ICE candidate, ready to be sent as a control message.
    std::function<void(const std::string& candidate, const std::string& mid)>
        on_local_candidate;
    /// The peer connection or ICE state changed (diagnostics + fallback).
    std::function<void(const char* state)> on_state;
    /// The data channel opened; the caller may now prefer it over the socket.
    std::function<void()> on_data_channel_open;
    std::function<void(const std::string& text)> on_text;
    std::function<void(const std::vector<uint8_t>& data)> on_binary;
    /// The receiver asked for a keyframe (PLI or FIR over RTCP). The capture
    /// should produce one on its next frame; a browser that just reconnected
    /// or lost packets cannot decode again until it does.
    std::function<void()> on_keyframe_request;
  };

  rtc_session();
  ~rtc_session();
  rtc_session(const rtc_session&) = delete;
  rtc_session& operator=(const rtc_session&) = delete;

  /// Builds the peer, the video track and the data channel, then starts
  /// gathering: the offer arrives through `on_local_description`.
  bool start(const config& cfg, callbacks cb, std::string* error);

  /// Applies the browser's answer.
  bool accept_answer(const std::string& sdp, std::string* error);
  void add_remote_candidate(const std::string& candidate, const std::string& mid);

  /// True once the data channel is open.
  bool channel_open() const;

  bool send_text(const std::string& text);
  bool send_binary(const uint8_t* data, std::size_t len);

  /// Sends one audio packet on the unreliable channel. False when the channel
  /// is not open, so the caller can fall back to the session transport.
  bool send_audio(const uint8_t* data, std::size_t len);
  bool audio_open() const;

  /// Sends one Annex-B access unit on the video track. `ts_us` stamps the
  /// frame; RTP timestamps run at 90 kHz.
  bool send_video(const uint8_t* annexb, std::size_t len, uint64_t ts_us);

  /// True when the video track is open and can carry frames.
  bool video_open() const;

  void close();

  /// Implementation detail; the definition lives in the .cpp.
  struct impl;

 private:
  /// Shared so callbacks installed on the peer can hold a weak reference and
  /// never outlive this object.
  std::shared_ptr<impl> impl_;
};

}  // namespace molink
