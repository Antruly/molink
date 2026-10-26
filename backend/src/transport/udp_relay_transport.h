#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "transport/relay_session.h"

struct sockaddr_storage;

namespace uvcpp {
class uvcpp_loop;
class uvcpp_udp;
class uvcpp_timer;
struct uvcpp_udp_send;
}

namespace molink {

/// A relay session carried by raw UDP datagrams.
///
/// The directory issues a per-session token; the client sends one JSON
/// datagram `{"connect_id":"...","token":"..."}` and retransmits it until the
/// server answers `{"type":"relay-ready"}` (or an error). After that, MOLK
/// packets travel as the same tagged datagrams the ICE transport uses:
///
///   0x00 text, 0x01 binary, 0x02 fragment (10-byte header, 1200-byte payload)
///
/// so large keyframes are split and reassembled here. Nothing is
/// retransmitted: loss is repaired by the session's tile-clock logic, exactly
/// like P2P. JSON notices always start with '{', which cannot collide with the
/// 0x00/0x01/0x02 data tags.
///
/// Threading: app loop only.
class udp_relay_transport final
    : public relay_session,
      public std::enable_shared_from_this<udp_relay_transport> {
 public:
  using post_fn = std::function<void(std::function<void()>)>;

  udp_relay_transport(std::string host, int port, std::string connect_id,
                      std::string token, uvcpp::uvcpp_loop* loop,
                      post_fn post);
  ~udp_relay_transport() override;

  const char* name() const override { return "udp"; }
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
  struct fragment_set {
    uint8_t kind = 0;
    uint16_t count = 0;
    uint16_t received = 0;
    uint64_t updated_ms = 0;
    std::vector<std::string> parts;
    std::vector<bool> have;
  };

  void send_handshake();
  void on_datagram(const uint8_t* data, std::size_t len);
  void handle_notice(const std::string& text);
  void handle_data(const uint8_t* data, std::size_t len);
  bool send_message(uint8_t kind, const uint8_t* data, std::size_t len);
  void send_datagram(const uint8_t* data, std::size_t len,
                     std::function<void(int)> done);
  /// Fragments are paced: a large keyframe is hundreds of datagrams, and
  /// blasting them overflows the relay's receive buffer (measured ~68% loss
  /// for a 1000-datagram burst). Queue and send a few per loop tick.
  void enqueue_datagram(std::string bytes);
  void pump_datagrams();
  void become_ready();
  void deliver_close(int code, const std::string& reason);
  void fail(const std::string& reason);

  std::string host_;
  int port_ = 0;
  std::string connect_id_;
  std::string token_;
  uvcpp::uvcpp_loop* loop_;
  post_fn post_;

  // Declared before the handles that reference it; the socket is destroyed
  // first.
  std::unique_ptr<struct sockaddr_storage> server_address_;
  int server_address_len_ = 0;
  std::unique_ptr<uvcpp::uvcpp_udp> udp_;
  std::unique_ptr<uvcpp::uvcpp_timer> retry_;
  std::unique_ptr<uvcpp::uvcpp_timer> pacer_;

  std::map<uint32_t, fragment_set> fragments_;
  uint32_t next_message_id_ = 1;
  std::deque<std::string> pending_;
  std::size_t pending_bytes_ = 0;
  bool pacer_running_ = false;

  bool started_ = false;
  bool ready_ = false;
  bool closed_ = false;
  bool close_delivered_ = false;

  text_handler text_;
  binary_handler binary_;
  close_handler on_close_;
  std::function<void()> ready_handler_;
};

}  // namespace molink
