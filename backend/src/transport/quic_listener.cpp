#include "transport/quic_listener.h"

#include <cstdint>
#include <string>
#include <utility>

#include <uvcpp/uvcpp_config.h>

#if UVCPP_QUIC_ENABLE
#include <net/uvcpp_net_read.h>
#include <quic/uvcpp_quic_connection.h>
#include <quic/uvcpp_quic_server.h>
#include <ssl/uvcpp_ssl_context.h>
#endif

namespace molink {

struct quic_listener::impl {
#if UVCPP_QUIC_ENABLE
  /// One accepted connection. QUIC can carry many streams; a MOLK session
  /// uses the first bidirectional one and ignores the rest.
  struct connection {
    std::shared_ptr<stream_transport> link;
    int64_t stream_id = -1;
  };

  accept_fn accept;
  uvcpp::uvcpp_ssl_context tls{uvcpp::tls_mode::SERVER};
  std::unique_ptr<uvcpp::uvcpp_quic_server> server;
  bool listening = false;
  int port = 0;
#endif
};

quic_listener::quic_listener(uvcpp::uvcpp_loop* loop, std::string cert_path,
                             std::string key_path, accept_fn accept)
    : loop_(loop),
      cert_path_(std::move(cert_path)),
      key_path_(std::move(key_path)),
      accept_(std::move(accept)) {}

quic_listener::~quic_listener() { stop(); }

bool quic_listener::start(const std::string& ip, int port, std::string& error) {
#if !UVCPP_QUIC_ENABLE
  (void)ip;
  (void)port;
  error = "libuvcpp was built without QUIC";
  return false;
#else
  if (impl_ && impl_->listening) return true;
  if (!impl_) impl_ = std::make_unique<impl>();
  impl_->accept = accept_;

  if (!impl_->tls.load_certificate_file(cert_path_)) {
    error = "could not load the device certificate";
    return false;
  }
  if (!impl_->tls.load_private_key_file(key_path_)) {
    error = "could not load the device private key";
    return false;
  }
  // QUIC has no cleartext mode: ALPN is part of the TLS handshake. "molink"
  // names the session protocol rather than HTTP/3 so an h3 client cannot
  // accidentally land here.
  impl_->tls.set_alpn_select_protos({"molink"});

  impl_->server = std::make_unique<uvcpp::uvcpp_quic_server>(loop_);
  impl_->server->set_ssl_context(&impl_->tls);
  impl_->server->set_alpn_select_protos({"molink"});

  const int bind_status = impl_->server->bind(ip.c_str(), port);
  if (bind_status != 0) {
    error = "could not register QUIC address " + ip + ":" +
            std::to_string(port) + " (" + std::to_string(bind_status) + ")";
    impl_->server.reset();
    return false;
  }

  impl* self = impl_.get();
  const int listen_status = impl_->server->listen(
      [self](uvcpp::uvcpp_quic_connection* connection) {
        if (connection == nullptr) return;
        auto state = std::make_shared<impl::connection>();
        uvcpp::uvcpp_quic_connection::callbacks callbacks;

        callbacks.on_stream_open =
            [self, state](uvcpp::uvcpp_quic_connection& conn,
                          int64_t stream_id) {
              if (state->link) {
                // One session per connection: refuse extra streams instead of
                // silently multiplexing a second client onto the first.
                conn.shutdown_stream_read(stream_id, 0);
                return;
              }
              state->stream_id = stream_id;
              // The connection object belongs to the endpoint and stays valid
              // until its on_close; capturing the pointer (not the reference
              // parameter) keeps that contract visible.
              uvcpp::uvcpp_quic_connection* connection = &conn;
              auto link = std::make_shared<stream_transport>(
                  [connection, stream_id](const uint8_t* data,
                                          std::size_t len) {
                    return connection->write_stream(
                               stream_id,
                               reinterpret_cast<const char*>(data), len,
                               false) == 0;
                  },
                  [connection](int, const std::string&) {
                    connection->close();
                  });
              state->link = link;
              if (self->accept) self->accept(link);
            };

        callbacks.on_read = [state](uvcpp::uvcpp_quic_connection&,
                                    int64_t stream_id,
                                    const uvcpp::net_read_result& result) {
          if (!state->link || stream_id != state->stream_id) return;
          if (result.event == uvcpp::net_read_event::DATA) {
            state->link->feed(
                reinterpret_cast<const uint8_t*>(result.data), result.size);
          } else if (result.event == uvcpp::net_read_event::PEER_CLOSED) {
            state->link->peer_closed("peer closed the QUIC stream");
          } else {
            state->link->peer_closed("QUIC stream read error");
          }
        };

        callbacks.on_close = [state](uvcpp::uvcpp_quic_connection&, int code) {
          if (!state->link) return;
          state->link->peer_closed("QUIC connection closed (" +
                                   std::to_string(code) + ")");
        };

        connection->set_callbacks(callbacks);
      });

  if (listen_status != 0) {
    error = "could not listen for QUIC on " + ip + " (" +
            std::to_string(listen_status) + ")";
    impl_->server.reset();
    return false;
  }

  impl_->listening = true;
  impl_->port = impl_->server->configured_port();
  return true;
#endif
}

void quic_listener::stop() {
#if UVCPP_QUIC_ENABLE
  if (!impl_ || !impl_->server) return;
  impl_->server->stop();
  impl_->server.reset();
  impl_->listening = false;
  impl_->port = 0;
#endif
}

bool quic_listener::listening() const {
#if UVCPP_QUIC_ENABLE
  return impl_ && impl_->listening;
#else
  return false;
#endif
}

int quic_listener::port() const {
#if UVCPP_QUIC_ENABLE
  return impl_ ? impl_->port : 0;
#else
  return 0;
#endif
}

}  // namespace molink
