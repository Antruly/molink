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
class uvcpp_quic_client;
class uvcpp_quic_connection;
class uvcpp_ssl_context;
}

namespace molink {

/// A relay session carried by one QUIC bidirectional stream.
///
/// Same handshake and framing as the TCP relay:
///
///   client -> {"connect_id":"...","token":"..."}\n
///   server -> {"type":"relay-ready"}\n   (or relay-error)
///
/// then the opaque MOLK stream (control messages already wrapped in
/// `packet_type::control`). ALPN is "molink-relay", distinct from the direct
/// QUIC session listener's "molink".
///
/// Threading: everything runs on the app loop that owns the QUIC client.
class quic_relay_transport final
    : public relay_session,
      public std::enable_shared_from_this<quic_relay_transport> {
 public:
  using post_fn = std::function<void(std::function<void()>)>;

  quic_relay_transport(std::string host, int port, std::string connect_id,
                       std::string token, std::string ca_path,
                       uvcpp::uvcpp_loop* loop, post_fn post);
  ~quic_relay_transport() override;

  const char* name() const override { return "quic"; }
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
  struct pending_write {
    std::size_t size = 0;
    std::function<void(int)> done;
  };

  void on_connected(int status);
  void on_read(const uint8_t* data, std::size_t len);
  void on_write(int status);
  void on_link_closed(const std::string& reason);
  void become_ready();
  void deliver_close(int code, const std::string& reason);
  void fail(const std::string& reason);
  bool write_bytes(const uint8_t* data, std::size_t len,
                   std::function<void(int)> done);
  void fail_pending();

  std::string host_;
  int port_ = 0;
  std::string connect_id_;
  std::string token_;
  std::string ca_path_;
  uvcpp::uvcpp_loop* loop_;
  post_fn post_;

  std::shared_ptr<uvcpp::uvcpp_ssl_context> tls_;
  std::shared_ptr<uvcpp::uvcpp_quic_client> client_;
  int64_t stream_id_ = 0;
  std::shared_ptr<stream_transport> inner_;
  std::deque<pending_write> writes_;
  std::size_t queued_ = 0;

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
