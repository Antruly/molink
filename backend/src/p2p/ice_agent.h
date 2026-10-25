#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_timer.h>
#include <handle/uvcpp_udp.h>
#include <juice/juice.h>

namespace molink {

/// One ICE agent with an application-owned UDP socket.
///
/// The socket is a libuvcpp `uvcpp_udp` bound to an ephemeral IPv4 port, so it
/// runs on the same libuv instance as the rest of the core. libjuice runs in
/// external-socket mode: no socket or thread of its own. Inbound datagrams are
/// fed to `juice_agent_input()`, outbound ICE packets go through `try_send`,
/// and a `uvcpp_timer` drives `juice_agent_tick()`. Application datagrams are
/// handed to the data callback unchanged.
///
/// Threading: every method and callback runs on the libuvcpp loop.
class ice_agent {
 public:
  using candidate_handler = std::function<void(const std::string& sdp)>;
  using gathering_handler = std::function<void()>;
  using state_handler = std::function<void(int state)>;
  using data_handler = std::function<void(const uint8_t* data, std::size_t len)>;

  explicit ice_agent(uvcpp::uvcpp_loop* loop);
  ~ice_agent();

  ice_agent(const ice_agent&) = delete;
  ice_agent& operator=(const ice_agent&) = delete;

  /// Bind the socket and start gathering. `stun_host` may be empty (host
  /// candidates only).
  bool start(const std::string& stun_host, uint16_t stun_port,
             std::string& error);
  void set_handlers(candidate_handler candidate, gathering_handler gathering,
                    state_handler state, data_handler data);
  void set_data_handler(data_handler data);

  bool apply_remote_offer(const std::string& sdp,
                          const std::vector<std::string>& candidates);
  void add_remote_candidate(const std::string& sdp);
  void remote_gathering_done();

  std::string local_description() const;
  int state() const;
  bool send(const uint8_t* data, std::size_t len);
  /// Remote endpoint of the selected pair, "ip:port"; empty until connected.
  std::string selected_remote() const;

  void close();

 private:
  static int sendto_callback(void* user_ptr, const void* data, std::size_t size,
                             const void* dst, int dstlen);
  static void on_state(juice_agent_t* agent, juice_state_t state,
                       void* user_ptr);
  static void on_candidate(juice_agent_t* agent, const char* sdp,
                           void* user_ptr);
  static void on_gathering_done(juice_agent_t* agent, void* user_ptr);
  void receive_datagram(const uint8_t* data, std::size_t len,
                        const struct sockaddr* src);
  void tick();
  void deliver_data(const uint8_t* data, std::size_t len);

  uvcpp::uvcpp_loop* loop_ = nullptr;
  juice_agent_t* agent_ = nullptr;
  std::unique_ptr<uvcpp::uvcpp_udp> udp_;
  std::unique_ptr<uvcpp::uvcpp_timer> timer_;
  bool started_ = false;
  bool closed_ = false;

  candidate_handler candidate_;
  gathering_handler gathering_;
  state_handler state_;
  data_handler data_;
};

}  // namespace molink
