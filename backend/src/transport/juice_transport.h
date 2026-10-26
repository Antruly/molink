#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "p2p/ice_agent.h"
#include "transport/session_transport.h"

namespace molink {

/// Session transport over an ICE-selected UDP pair.
///
/// Each datagram carries one byte of framing followed by the payload:
///   0x00 + JSON text control message
///   0x01 + one MOLK binary packet
///
/// The browser side is bridged by the local core's /ws/connect proxy; the
/// remote core decodes the same framing in its session controller. ICE
/// datagrams are consumed by libjuice and never reach this layer.
class juice_transport final : public session_transport {
 public:
  juice_transport(std::shared_ptr<ice_agent> agent, std::string peer_ip,
                  unsigned int peer_port);
  ~juice_transport() override;

  const char* name() const override { return "udp"; }
  const char* path() const override { return "p2p"; }
  std::string peer_ip() const override { return peer_ip_; }
  unsigned int peer_port() const override { return peer_port_; }

  bool is_open() const override;
  bool send_text(const std::string& text) override;
  bool send_binary(const uint8_t* data, std::size_t len) override;
  void close(int code, const std::string& reason) override;
  void set_handlers(text_handler on_text, binary_handler on_binary,
                    close_handler on_close) override;

  /// Called by the ICE agent for every non-ICE datagram.
  void handle_datagram(const uint8_t* data, std::size_t len);
  /// Called when the ICE agent fails or the selected pair is torn down.
  void notify_closed(int code, const std::string& reason);
  std::shared_ptr<ice_agent> agent() const { return agent_; }

 private:
  bool send_message(uint8_t kind, const uint8_t* data, std::size_t len);

  struct fragment_set {
    uint8_t kind = 0;
    uint16_t count = 0;
    std::size_t received_count = 0;
    std::vector<std::string> parts;
    std::vector<bool> received;
  };
  std::shared_ptr<ice_agent> agent_;
  std::string peer_ip_;
  unsigned int peer_port_ = 0;
  bool closed_ = false;
  uint32_t next_message_id_ = 1;
  std::map<uint32_t, fragment_set> fragments_;

  text_handler text_;
  binary_handler binary_;
  close_handler close_;
};

}  // namespace molink
