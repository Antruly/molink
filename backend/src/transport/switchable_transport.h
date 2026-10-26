#pragma once

#include <functional>
#include <memory>
#include <string>

#include "transport/session_transport.h"

namespace molink {

/// A session transport that starts on the relay and switches to the direct
/// ICE path once hole punching completes. Deltas, input and ordinary control
/// text use ICE; file data, keyframes, AV1 patches, tile refreshes, the tile
/// clock and repair requests stay on the reliable relay in both directions.
class switchable_transport final : public session_transport {
 public:
  explicit switchable_transport(std::shared_ptr<session_transport> initial);

  const char* name() const override;
  const char* path() const override;
  std::string peer_ip() const override;
  unsigned int peer_port() const override;

  bool is_open() const override;
  bool send_text(const std::string& text) override;
  bool send_binary(const uint8_t* data, std::size_t len) override;
  void close(int code, const std::string& reason) override;
  void set_handlers(text_handler on_text, binary_handler on_binary,
                    close_handler on_close) override;

  /// Install the completed ICE transport and switch to it. Safe to call
  /// after the relay is already in use; the relay is kept for the reliable
  /// subset above.
  void set_ice(std::shared_ptr<session_transport> ice);
  /// Called after the session moved to the ICE path (app loop).
  void set_switch_handler(std::function<void()> handler);

  bool switched() const { return switched_; }
  std::shared_ptr<session_transport> relay() const { return relay_; }

 private:
  void wire(const std::shared_ptr<session_transport>& transport);

  std::shared_ptr<session_transport> active_;
  std::shared_ptr<session_transport> relay_;
  std::shared_ptr<session_transport> ice_;
  text_handler text_;
  binary_handler binary_;
  close_handler close_;
  bool switched_ = false;
  bool closed_ = false;
  std::function<void()> switch_handler_;
};

}  // namespace molink
