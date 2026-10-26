#include "transport/tcp_listener.h"

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <handle/uvcpp_loop.h>
#include <net/uvcpp_net_read.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>
#include <ssl/uvcpp_ssl_context.h>

namespace molink {

namespace {

/// A slow reader must not make the host buffer an unbounded stream: the
/// session would rather close (the client reconnects and negotiates another
/// transport) than grow until the process is OOM-killed.
constexpr std::size_t kMaxSendBacklogBytes = 8u * 1024u * 1024u;

}  // namespace

struct tcp_listener::impl {
  struct connection {
    uvcpp::uvcpp_tcp_client* client = nullptr;
    std::shared_ptr<stream_transport> link;
    std::deque<std::string> queue;
    std::size_t queued = 0;
    bool writing = false;
    bool closed = false;
  };

  /// uvcpp_tcp_client allows only one async write in flight and rejects
  /// rather than queues a second one, so the transport keeps its own FIFO.
  static void pump(const std::shared_ptr<connection>& state) {
    if (state->writing || state->queue.empty() || state->closed) return;
    if (state->client == nullptr) {
      state->queue.clear();
      state->queued = 0;
      return;
    }
    auto chunk = std::make_shared<std::string>(std::move(state->queue.front()));
    state->queue.pop_front();
    const std::size_t size = chunk->size();
    state->writing = true;
    uvcpp::uvcpp_tcp_client* client = state->client;
    const int status = client->write(
        chunk->data(), chunk->size(),
        [state, chunk, size](int write_status) {
          state->writing = false;
          state->queued -= size;
          if (write_status != 0) {
            state->closed = true;
            state->queue.clear();
            state->queued = 0;
            uvcpp::uvcpp_tcp_client* failed = state->client;
            state->client = nullptr;
            if (failed != nullptr) {
              // Deliver the close after the handle finishes closing: calling
              // the session's close handler from inside its own send path
              // would destroy the controller mid-call.
              failed->close([state] {
                if (state->link) {
                  state->link->peer_closed("TCP write failed");
                }
              });
            }
            return;
          }
          pump(state);
        });
    if (status != 0) {
      // Not submitted: the completion callback will not run.
      state->writing = false;
      state->queued -= size;
      state->closed = true;
      state->queue.clear();
      state->queued = 0;
      uvcpp::uvcpp_tcp_client* failed = state->client;
      state->client = nullptr;
      if (failed != nullptr) {
        failed->close([state] {
          if (state->link) state->link->peer_closed("TCP write failed");
        });
      }
    }
  }

  accept_fn accept;
  std::unique_ptr<uvcpp::uvcpp_ssl_context> tls;
  std::unique_ptr<uvcpp::uvcpp_tcp_server> server;
  /// Accepted handles, removed when their close callback runs, so the table
  /// tracks live connections only.
  std::map<uvcpp::uvcpp_tcp_client*, std::weak_ptr<connection>> live;
  bool listening = false;
  int port = 0;
};

tcp_listener::tcp_listener(uvcpp::uvcpp_loop* loop, std::string cert_path,
                           std::string key_path, accept_fn accept)
    : loop_(loop),
      cert_path_(std::move(cert_path)),
      key_path_(std::move(key_path)),
      accept_(std::move(accept)) {}

tcp_listener::~tcp_listener() { stop(); }

bool tcp_listener::start(const std::string& ip, int port, std::string& error) {
  if (impl_ && impl_->listening) return true;
  if (!impl_) impl_ = std::make_unique<impl>();
  impl_->accept = accept_;
  if (cert_path_.empty()) {
    error = "TCP data sessions need the device certificate";
    return false;
  }
  impl_->tls =
      std::make_unique<uvcpp::uvcpp_ssl_context>(uvcpp::tls_mode::SERVER);
  if (!impl_->tls->load_certificate_file(cert_path_)) {
    error = "could not load the device certificate";
    return false;
  }
  if (!impl_->tls->load_private_key_file(key_path_)) {
    error = "could not load the device private key";
    return false;
  }

  impl_->server = std::make_unique<uvcpp::uvcpp_tcp_server>(loop_);
  impl_->server->set_ssl_context(impl_->tls.get());
  const int bind_status = impl_->server->bind(ip.c_str(), port);
  if (bind_status != 0) {
    error = "could not bind " + ip + ":" + std::to_string(port) + " (" +
            std::to_string(bind_status) + ")";
    impl_->server.reset();
    return false;
  }

  impl* self = impl_.get();
  const int listen_status = impl_->server->listen(
      [self](uvcpp::uvcpp_tcp_client* client) {
        if (client == nullptr) return;
        auto state = std::make_shared<impl::connection>();
        state->client = client;

        std::string peer_ip;
        int peer_port = 0;
        client->getPeerAddrs(peer_ip, peer_port);

        std::shared_ptr<stream_transport> link =
            std::make_shared<stream_transport>(
                [state](const uint8_t* data, std::size_t len) {
                  if (state->closed || state->client == nullptr) return false;
                  if (state->queued + len > kMaxSendBacklogBytes) {
                    // Close asynchronously; the pending close callback tells
                    // the session why.
                    state->closed = true;
                    state->queue.clear();
                    state->queued = 0;
                    uvcpp::uvcpp_tcp_client* slow = state->client;
                    state->client = nullptr;
                    if (slow != nullptr) {
                      slow->close([state] {
                        if (state->link) {
                          state->link->peer_closed(
                              "TCP send backlog exceeded");
                        }
                      });
                    }
                    return false;
                  }
                  state->queue.emplace_back(
                      reinterpret_cast<const char*>(data), len);
                  state->queued += len;
                  impl::pump(state);
                  return true;
                },
                [state](int, const std::string&) {
                  if (state->closed) return;
                  state->closed = true;
                  state->queue.clear();
                  state->queued = 0;
                  uvcpp::uvcpp_tcp_client* active = state->client;
                  state->client = nullptr;
                  if (active != nullptr) active->close();
                },
                "tcp", std::move(peer_ip),
                static_cast<unsigned int>(peer_port));
        state->link = link;
        self->live[client] = state;

        client->set_on_close([self, client, state] {
          self->live.erase(client);
          state->closed = true;
          state->client = nullptr;
          state->queue.clear();
          state->queued = 0;
          if (state->link) state->link->peer_closed("peer closed");
        });
        client->read_start_events(
            [state](uvcpp::uvcpp_tcp_client&, const uvcpp::net_read_result& r) {
              if (r.event != uvcpp::net_read_event::DATA) return;
              if (!state->closed && state->link) {
                state->link->feed(
                    reinterpret_cast<const uint8_t*>(r.data), r.size);
              }
            });

        if (self->accept) self->accept(link);
      });

  if (listen_status != 0) {
    error = "could not listen on " + ip + ":" + std::to_string(port) + " (" +
            std::to_string(listen_status) + ")";
    impl_->server.reset();
    return false;
  }
  impl_->listening = true;
  impl_->port = port;
  return true;
}

void tcp_listener::stop() {
  if (!impl_ || !impl_->server) return;
  impl_->listening = false;
  // Close accepted clients first so no session keeps writing to a handle the
  // server is about to destroy. Collect them before closing: the close
  // callback erases from live.
  std::vector<uvcpp::uvcpp_tcp_client*> clients;
  for (auto& entry : impl_->live) {
    if (auto state = entry.second.lock()) {
      if (state->client != nullptr) {
        state->closed = true;
        state->client = nullptr;
        state->queue.clear();
        state->queued = 0;
        clients.push_back(entry.first);
      }
    }
  }
  impl_->live.clear();
  for (uvcpp::uvcpp_tcp_client* client : clients) client->close();
  impl_->server->stop();
  impl_->server.reset();
  impl_->tls.reset();
  impl_->port = 0;
}

bool tcp_listener::listening() const {
  return impl_ && impl_->listening;
}

int tcp_listener::port() const { return impl_ ? impl_->port : 0; }

}  // namespace molink
