#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "transport/session_transport.h"
#include "transport/wt_framing.h"

namespace uvcpp {
class uvcpp_h3_wt_session;
class uvcpp_h3_wt_stream;
}  // namespace uvcpp

namespace molink {

class stream_transport;

/// A WebTransport session as a session_transport (Phase 5).
///
/// Reliable traffic -- control, audio and file -- rides one bidirectional
/// stream as bare MOLK packets; the stream is a byte pipe, so both ends
/// delimit packets by the MOLK header's payload length, exactly like the QUIC
/// data stream.
///
/// Video rides H3 datagrams. They never head-of-line block the reliable
/// stream, which is what keeps a file transfer from delaying the picture.
class wt_transport final : public session_transport {
 public:
  explicit wt_transport(std::string peer = "webtransport");
  ~wt_transport() override;

  /// Takes the accepted session (not owned). The reliable stream is the one
  /// the peer opens; `attach_stream` wires it up when it arrives.
  void attach(uvcpp::uvcpp_h3_wt_session* session);

  /// Takes the peer's bidirectional stream (not owned) as the reliable path.
  void attach_stream(uvcpp::uvcpp_h3_wt_stream* stream);

  /// The session or its stream ended.
  void link_closed(int code, const std::string& reason);

  const char* name() const override { return "webtransport"; }
  const char* path() const override { return "direct"; }
  std::string peer_ip() const override;
  unsigned int peer_port() const override { return 0; }
  bool is_open() const override;
  bool send_text(const std::string& text) override;
  bool send_text(transport_channel channel, const std::string& text) override;
  bool send_binary(const uint8_t* data, std::size_t len) override;
  bool send_binary(transport_channel channel, const uint8_t* data,
                   std::size_t len) override;
  void close(int code, const std::string& reason) override;
  void set_handlers(text_handler on_text, binary_handler on_binary,
                    close_handler on_close) override;

  /// Called from the session callbacks.
  void feed_reliable(const uint8_t* data, std::size_t len);
  void feed_datagram(const uint8_t* data, std::size_t len);

  /// Frames reassembled from datagrams so far (diagnostics and tests).
  std::size_t frames_received() const { return frames_received_; }

 private:
  void deliver_close(int code, const std::string& reason);

  std::string peer_;
  uvcpp::uvcpp_h3_wt_session* session_ = nullptr;
  uvcpp::uvcpp_h3_wt_stream* reliable_ = nullptr;
  std::shared_ptr<stream_transport> stream_;
  datagram_reassembler reassembly_;
  uint32_t next_frame_ = 1;
  std::size_t frames_received_ = 0;
  bool closed_ = false;
  bool close_delivered_ = false;

  text_handler text_;
  binary_handler binary_;
  close_handler close_;
};

}  // namespace molink
