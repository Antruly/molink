#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>

#include "transport/relay_session.h"
#include "transport/stream_transport.h"

namespace uvcpp {
class uvcpp_loop;
class uvcpp_tcp_client;
class uvcpp_ssl_context;
}

namespace molink {

/// A relay session carried by a raw stream (TCP now, QUIC next) instead of
/// the directory's WebSocket.
///
/// The server endpoint pairs exactly like `WS /v1/relay`, but the handshake is
/// one JSON line:
///
///   client -> {"connect_id":"...","token":"..."}\n
///   server -> {"type":"relay-ready"}\n   (or relay-error)
///
/// After that line the stream is the same self-delimiting MOLK stream as the
/// direct TCP/QUIC transports: JSON control messages ride in
/// `packet_type::control`, and the relay forwards the bytes untouched.
///
/// Threading: everything runs on the app loop that owns the socket. The
/// WebSocket relay_transport posts from its own thread; this one does not.
class relay_stream_transport final : public relay_session,
                                     public std::enable_shared_from_this<
                                         relay_stream_transport> {
 public:
  using post_fn = std::function<void(std::function<void()>)>;

  relay_stream_transport(std::string protocol, std::string host, int port,
                         std::string connect_id, std::string token,
                         std::string ca_path, bool secure,
                         uvcpp::uvcpp_loop* loop, post_fn post);
  ~relay_stream_transport() override;

  const char* name() const override { return protocol_.c_str(); }
  const char* path() const override { return "relay"; }
  std::string peer_ip() const override { return "relay"; }
  unsigned int peer_port() const override { return 0; }

  bool is_open() const override;
  bool send_text(const std::string& text) override;
  bool send_binary(const uint8_t* data, std::size_t len) override;
  void close(int code, const std::string& reason) override;
  void set_handlers(text_handler on_text, binary_handler on_binary,
                    close_handler on_close) override;
  void set_ready_handler(std::function<void()> ready) override;
  void start() override;

 private:
  struct link_state;
  struct outbound {
    std::string bytes;
    std::function<void(int)> done;
  };

  void on_connected(int status);
  void on_read(const uint8_t* data, std::size_t len);
  void on_link_closed(const std::string& reason);
  bool parse_handshake();
  void become_ready();
  void deliver_close(int code, const std::string& reason);
  void fail(const std::string& reason);

  void enqueue(std::string bytes, std::function<void(int)> done);
  void pump();
  void fail_pending();

  std::string protocol_;
  std::string host_;
  int port_ = 0;
  std::string connect_id_;
  std::string token_;
  std::string ca_path_;
  bool secure_ = true;
  uvcpp::uvcpp_loop* loop_;
  post_fn post_;

  std::shared_ptr<link_state> state_;
  std::shared_ptr<uvcpp::uvcpp_ssl_context> tls_;
  std::shared_ptr<uvcpp::uvcpp_tcp_client> client_;
  std::shared_ptr<stream_transport> inner_;

  std::deque<outbound> queue_;
  std::size_t queued_ = 0;
  bool writing_ = false;
  std::string handshake_;
  bool ready_ = false;
  bool started_ = false;
  bool closed_ = false;
  bool close_delivered_ = false;

  text_handler text_;
  binary_handler binary_;
  close_handler on_close_;
  std::function<void()> ready_handler_;
};

}  // namespace molink
