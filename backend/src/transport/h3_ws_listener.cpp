#include "transport/h3_ws_listener.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include <uvcpp/uvcpp_config.h>

#ifndef MOLINK_HAVE_H3_WS
// Set by CMake when the installed libuvcpp ships the HTTP/3 WebSocket header.
#define MOLINK_HAVE_H3_WS 0
#endif

#if MOLINK_HAVE_H3_WS
#include <http3/uvcpp_h3_common.h>
#include <http3/uvcpp_h3_connection.h>
#include <http3/uvcpp_h3_ws_connection.h>
#include <quic/uvcpp_quic_connection.h>
#include <quic/uvcpp_quic_server.h>
#include <ssl/uvcpp_ssl_context.h>

#include "transport/h3_ws_transport.h"
#endif

namespace molink {

struct h3_ws_listener::impl {
#if MOLINK_HAVE_H3_WS
  struct connection_state {
    std::unique_ptr<uvcpp::uvcpp_h3_connection> h3;
    std::unique_ptr<uvcpp::uvcpp_h3_ws_connection> ws;
    std::shared_ptr<h3_ws_transport> transport;
    bool closed = false;
  };

  impl(uvcpp::uvcpp_loop* loop, std::string cert, std::string key,
       accept_fn accept, post_fn post)
      : loop(loop),
        cert(std::move(cert)),
        key(std::move(key)),
        accept(std::move(accept)),
        post(std::move(post)) {}

  uvcpp::uvcpp_loop* loop;
  std::string cert;
  std::string key;
  accept_fn accept;
  post_fn post;
  std::unique_ptr<uvcpp::uvcpp_ssl_context> tls;
  std::unique_ptr<uvcpp::uvcpp_quic_server> server;
  std::map<uvcpp::uvcpp_quic_connection*,
           std::shared_ptr<connection_state>>
      connections;
  bool listening = false;
  int port = 0;
#else
  impl(uvcpp::uvcpp_loop*, std::string, std::string, accept_fn, post_fn) {}
#endif
};

h3_ws_listener::h3_ws_listener(uvcpp::uvcpp_loop* loop, std::string cert_path,
                               std::string key_path, accept_fn accept,
                               post_fn post)
    : impl_(std::make_shared<impl>(loop, std::move(cert_path),
                                   std::move(key_path), std::move(accept),
                                   std::move(post))) {}

h3_ws_listener::~h3_ws_listener() { stop(); }

bool h3_ws_listener::start(const std::string& ip, int port,
                           std::string& error) {
#if !MOLINK_HAVE_H3_WS
  (void)ip;
  (void)port;
  error = "libuvcpp was built without HTTP/3";
  return false;
#else
  if (impl_->listening) return true;
  if (impl_->cert.empty()) {
    error = "HTTP/3 needs TLS (no device certificate)";
    return false;
  }
  impl_->tls =
      std::make_unique<uvcpp::uvcpp_ssl_context>(uvcpp::tls_mode::SERVER);
  if (!impl_->tls->load_certificate_file(impl_->cert)) {
    error = "could not load the device certificate";
    return false;
  }
  if (!impl_->tls->load_private_key_file(impl_->key)) {
    error = "could not load the device private key";
    return false;
  }
  impl_->tls->set_alpn_select_protos({"h3"});

  impl_->server = std::make_unique<uvcpp::uvcpp_quic_server>(impl_->loop);
  impl_->server->set_ssl_context(impl_->tls.get());
  impl_->server->set_alpn_select_protos({"h3"});
  const int bind_status = impl_->server->bind(ip.c_str(), port);
  if (bind_status != 0) {
    error = "could not register " + ip + ":" + std::to_string(port) + " (" +
            std::to_string(bind_status) + ")";
    impl_->server.reset();
    return false;
  }

  std::shared_ptr<impl> self = impl_;
  const int listen_status = impl_->server->listen(
      [self](uvcpp::uvcpp_quic_connection* connection) {
        if (connection == nullptr) return;
        auto state = std::make_shared<impl::connection_state>();
        state->h3 =
            std::make_unique<uvcpp::uvcpp_h3_connection>(connection, true);
        self->connections[connection] = state;

        std::weak_ptr<impl> weak_self = self;
        std::weak_ptr<impl::connection_state> weak_state = state;
        uvcpp::uvcpp_h3_connection::callbacks callbacks;
        callbacks.on_connect_request =
            [weak_self, weak_state](uvcpp::uvcpp_h3_connection& h3,
                                    const uvcpp::h3_request& request) {
              auto state = weak_state.lock();
              if (!state || state->closed) return;
              if (request.protocol != "websocket") return;
              auto transport = std::make_shared<h3_ws_transport>("http3");
              std::weak_ptr<h3_ws_transport> weak_transport = transport;
              uvcpp::uvcpp_h3_ws_connection::callbacks ws_callbacks;
              ws_callbacks.on_text = [weak_transport](
                                         const std::string& text) {
                if (auto link = weak_transport.lock()) {
                  link->deliver_text(text);
                }
              };
              ws_callbacks.on_binary = [weak_transport](const uint8_t* data,
                                                        std::size_t len) {
                if (auto link = weak_transport.lock()) {
                  link->deliver_binary(data, len);
                }
              };
              ws_callbacks.on_close = [weak_transport](
                                          uvcpp::ws_close_code code,
                                          const std::string& reason) {
                if (auto link = weak_transport.lock()) {
                  link->link_closed(static_cast<int>(code), reason);
                }
              };
              ws_callbacks.on_error = [weak_transport](
                                          int code,
                                          const std::string& reason) {
                if (auto link = weak_transport.lock()) {
                  link->link_closed(code, reason);
                }
              };
              auto ws = uvcpp::uvcpp_h3_ws_connection::accept(h3, request,
                                                              ws_callbacks);
              if (!ws) {
                transport->link_closed(transport_close::policy,
                                       "h3 websocket rejected");
                return;
              }
              state->ws = std::move(ws);
              state->transport = transport;
              transport->attach(state->ws.get());
              if (auto owner = weak_self.lock()) {
                if (owner->accept) owner->accept(transport);
              }
            };
        callbacks.on_disconnect = [weak_self, weak_state, connection](
                                      uvcpp::uvcpp_h3_connection&) {
          auto state = weak_state.lock();
          if (state) {
            state->closed = true;
            if (state->transport) {
              state->transport->link_closed(transport_close::normal,
                                            "h3 connection closed");
            }
            state->ws.reset();
          }
          // Destroy the h3 object after this callback returns: it must not
          // be freed while the library is still on its stack.
          auto owner = weak_self.lock();
          if (owner && owner->post) {
            owner->post([weak_self, connection] {
              if (auto alive = weak_self.lock()) {
                alive->connections.erase(connection);
              }
            });
          }
        };
        state->h3->start(callbacks);
      });

  if (listen_status != 0) {
    error = "could not listen on " + ip + ":" + std::to_string(port) + " (" +
            std::to_string(listen_status) + ")";
    impl_->server.reset();
    impl_->tls.reset();
    return false;
  }
  impl_->listening = true;
  impl_->port = impl_->server->configured_port();
  return true;
#endif
}

void h3_ws_listener::stop() {
#if MOLINK_HAVE_H3_WS
  if (!impl_ || !impl_->server) return;
  impl_->listening = false;
  // Destroy the h3/WebSocket objects while the QUIC connections still exist.
  impl_->connections.clear();
  impl_->server->stop();
  impl_->server.reset();
  impl_->tls.reset();
  impl_->port = 0;
#endif
}

bool h3_ws_listener::listening() const {
#if MOLINK_HAVE_H3_WS
  return impl_ && impl_->listening;
#else
  return false;
#endif
}

int h3_ws_listener::port() const {
#if MOLINK_HAVE_H3_WS
  return impl_ ? impl_->port : 0;
#else
  return 0;
#endif
}

}  // namespace molink
