#include "p2p/ice_agent.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <req/uvcpp_udp_send.h>
#include <uvcpp/uvcpp_buf.h>

namespace molink {
namespace {

constexpr int kTickMs = 50;
constexpr std::size_t kMaxDatagram = 65536;

}  // namespace

ice_agent::ice_agent(uvcpp::uvcpp_loop* loop) : loop_(loop) {
  // A fixed port is what a firewall rule can admit. libjuice never binds a
  // socket itself in external mode, so this is also the port it advertises
  // through STUN.
  const char* port = std::getenv("MOLINK_P2P_PORT");
  if (port != nullptr && *port != '\0') {
    const int value = std::atoi(port);
    if (value > 0 && value <= 65535) {
      bind_port_ = static_cast<uint16_t>(value);
    }
  }
}

ice_agent::~ice_agent() { close(); }

bool ice_agent::start(const std::string& stun_host, uint16_t stun_port,
                      std::string& error) {
  if (started_ || loop_ == nullptr) {
    error = "ICE agent already started";
    return false;
  }
  udp_ = std::make_unique<uvcpp::uvcpp_udp>(loop_);
  struct sockaddr_in bind_address;
  std::memset(&bind_address, 0, sizeof(bind_address));
  bind_address.sin_family = AF_INET;
  bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
  bind_address.sin_port = htons(bind_port_);
  if (udp_->bind(reinterpret_cast<const struct sockaddr*>(&bind_address), 0) !=
      0) {
    error = bind_port_ == 0
                ? "cannot bind the P2P UDP socket"
                : "cannot bind the P2P UDP socket on port " +
                      std::to_string(bind_port_);
    return false;
  }
  struct sockaddr_storage bound;
  std::memset(&bound, 0, sizeof(bound));
  int bound_len = sizeof(bound);
  if (udp_->getsockname(reinterpret_cast<struct sockaddr*>(&bound),
                        &bound_len) != 0) {
    error = "cannot read the P2P UDP port";
    return false;
  }
  uint16_t port = 0;
  if (bound.ss_family == AF_INET) {
    port = ntohs(reinterpret_cast<struct sockaddr_in*>(&bound)->sin_port);
  }
  if (port == 0) {
    error = "the P2P UDP socket has no port";
    return false;
  }
  if (udp_->recv_start(
          [](uvcpp::uvcpp_handle*, std::size_t size, uv_buf_t* buf) {
            uvcpp::uvcpp_buf::alloc_buf(
                buf, size > 0 ? size : kMaxDatagram);
          },
          [this](uvcpp::uvcpp_udp*, ssize_t nread, const uv_buf_t* buf,
                 const struct sockaddr* addr, unsigned int) {
            if (nread > 0 && buf->base != nullptr && addr != nullptr) {
              receive_datagram(reinterpret_cast<const uint8_t*>(buf->base),
                               static_cast<std::size_t>(nread), addr);
            }
            if (buf->base != nullptr) uvcpp::uvcpp_free_bytes(buf->base);
          }) != 0) {
    error = "cannot receive on the P2P UDP socket";
    return false;
  }

  juice_config_t config;
  std::memset(&config, 0, sizeof(config));
  config.concurrency_mode = JUICE_CONCURRENCY_MODE_EXTERNAL;
  config.external.sendto = &ice_agent::sendto_callback;
  config.external.user_ptr = this;
  config.external.local_address = nullptr;
  config.external.local_port = port;
  config.external.address_family = JUICE_ADDRESS_FAMILY_IPV4;
  config.cb_state_changed = &ice_agent::on_state;
  config.cb_candidate = &ice_agent::on_candidate;
  config.cb_gathering_done = &ice_agent::on_gathering_done;
  config.user_ptr = this;
  if (!stun_host.empty()) {
    config.stun_server_host = stun_host.c_str();
    config.stun_server_port = stun_port == 0 ? 3478 : stun_port;
  }

  agent_ = juice_create(&config);
  if (agent_ == nullptr) {
    error = "cannot create the ICE agent";
    return false;
  }
  started_ = true;
  if (juice_gather_candidates(agent_) != 0) {
    error = "cannot gather ICE candidates";
    close();
    return false;
  }
  timer_ = std::make_unique<uvcpp::uvcpp_timer>(loop_);
  timer_->start([this](uvcpp::uvcpp_timer*) { tick(); }, kTickMs, kTickMs);
  return true;
}

void ice_agent::set_handlers(candidate_handler candidate,
                             gathering_handler gathering,
                             state_handler state, data_handler data) {
  candidate_ = std::move(candidate);
  gathering_ = std::move(gathering);
  state_ = std::move(state);
  data_ = std::move(data);
}

void ice_agent::set_data_handler(data_handler data) {
  data_ = std::move(data);
}

bool ice_agent::apply_remote_offer(
    const std::string& sdp, const std::vector<std::string>& candidates) {
  if (agent_ == nullptr || sdp.empty()) return false;
  // juice_get_local_description() already embeds every gathered candidate and
  // end-of-candidates; adding the separate trickle list again is rejected by
  // libjuice, so the SDP is the single source of truth here.
  (void)candidates;
  return juice_set_remote_description(agent_, sdp.c_str()) == 0;
}

void ice_agent::add_remote_candidate(const std::string& sdp) {
  if (agent_ != nullptr && !sdp.empty()) {
    juice_add_remote_candidate(agent_, sdp.c_str());
  }
}

void ice_agent::remote_gathering_done() {
  if (agent_ != nullptr) juice_set_remote_gathering_done(agent_);
}

std::string ice_agent::local_description() const {
  if (agent_ == nullptr) return {};
  char buffer[JUICE_MAX_SDP_STRING_LEN];
  buffer[0] = '\0';
  if (juice_get_local_description(agent_, buffer, sizeof(buffer)) != 0) {
    return {};
  }
  return std::string(buffer);
}

int ice_agent::state() const {
  return agent_ == nullptr ? JUICE_STATE_DISCONNECTED
                           : static_cast<int>(juice_get_state(agent_));
}

bool ice_agent::send(const uint8_t* data, std::size_t len) {
  if (agent_ == nullptr || data == nullptr) return false;
  return juice_send(agent_, reinterpret_cast<const char*>(data), len) ==
         JUICE_ERR_SUCCESS;
}

std::string ice_agent::selected_remote() const {
  if (agent_ == nullptr) return {};
  char local[JUICE_MAX_ADDRESS_STRING_LEN];
  char remote[JUICE_MAX_ADDRESS_STRING_LEN];
  local[0] = '\0';
  remote[0] = '\0';
  if (juice_get_selected_addresses(agent_, local, sizeof(local), remote,
                                   sizeof(remote)) != 0) {
    return {};
  }
  return std::string(remote);
}

void ice_agent::receive_datagram(const uint8_t* data, std::size_t len,
                                 const struct sockaddr* src) {
  if (agent_ == nullptr) return;
  const int consumed = juice_agent_input(agent_, data, len, src,
                                         sizeof(struct sockaddr_storage));
  if (consumed == 0) deliver_data(data, len);
}

void ice_agent::tick() {
  if (agent_ != nullptr) juice_agent_tick(agent_);
}

void ice_agent::deliver_data(const uint8_t* data, std::size_t len) {
  if (data_ && data != nullptr && len > 0) data_(data, len);
}

namespace {

struct udp_send_payload {
  std::vector<uint8_t> data;
  uv_buf_t buffer;
};

void on_udp_send(uvcpp::uvcpp_udp_send* request, int status) {
  (void)status;
  auto* payload = static_cast<udp_send_payload*>(request->get_data());
  delete payload;
  delete request;
}

}  // namespace

int ice_agent::sendto_callback(void* user_ptr, const void* data,
                               std::size_t size, const void* dst, int dstlen) {
  (void)dstlen;
  auto* agent = static_cast<ice_agent*>(user_ptr);
  if (agent == nullptr || agent->udp_ == nullptr || agent->closed_ ||
      dst == nullptr) {
    return -1;
  }
  // libuvcpp's async send queues in libuv, so a large fragmented keyframe is
  // paced by the loop instead of failing with EAGAIN.
  auto* payload = new udp_send_payload();
  payload->data.assign(static_cast<const uint8_t*>(data),
                       static_cast<const uint8_t*>(data) + size);
  payload->buffer.base = reinterpret_cast<char*>(payload->data.data());
  payload->buffer.len = payload->data.size();
  auto* request = new uvcpp::uvcpp_udp_send();
  request->init();
  request->set_uv_buf(&payload->buffer, false);
  request->set_data(payload);
  const int rc = agent->udp_->send(
      request, &payload->buffer, 1,
      static_cast<const struct sockaddr*>(dst), on_udp_send);
  if (rc != 0) {
    delete payload;
    delete request;
    return -1;
  }
  return 0;
}

void ice_agent::on_state(juice_agent_t*, juice_state_t state,
                         void* user_ptr) {
  auto* agent = static_cast<ice_agent*>(user_ptr);
  if (agent != nullptr && agent->state_) {
    agent->state_(static_cast<int>(state));
  }
}

void ice_agent::on_candidate(juice_agent_t*, const char* sdp, void* user_ptr) {
  auto* agent = static_cast<ice_agent*>(user_ptr);
  if (agent != nullptr && agent->candidate_ && sdp != nullptr) {
    agent->candidate_(sdp);
  }
}

void ice_agent::on_gathering_done(juice_agent_t*, void* user_ptr) {
  auto* agent = static_cast<ice_agent*>(user_ptr);
  if (agent != nullptr && agent->gathering_) agent->gathering_();
}

void ice_agent::close() {
  if (closed_) return;
  closed_ = true;
  if (timer_) {
    timer_->stop();
    timer_.reset();
  }
  udp_.reset();
  if (agent_ != nullptr) {
    juice_destroy(agent_);
    agent_ = nullptr;
  }
  candidate_ = nullptr;
  gathering_ = nullptr;
  state_ = nullptr;
  data_ = nullptr;
}

}  // namespace molink
