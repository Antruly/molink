// QUIC transport loopback: a QUIC client opens a stream to a QUIC server on
// the loopback interface, both sides carry MOLINK packets, and a JSON hello
// plus a binary ping round-trip through stream_transport. Built only when
// libuvcpp has QUIC and OpenSSL is available; otherwise the test is skipped.
// The config header comes first so a libuvcpp built without QUIC still
// compiles this file (and reports a skip) even if it did not install the
// <quic/...> headers.
#include <uvcpp/uvcpp_config.h>

#include <cstdio>

#if !UVCPP_QUIC_ENABLE
int main() {
  std::printf("SKIP: libuvcpp was built without QUIC\n");
  return 77;
}
#else

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_timer.h>
#include <net/uvcpp_net_read.h>
#include <quic/uvcpp_quic_client.h>
#include <quic/uvcpp_quic_connection.h>
#include <ssl/uvcpp_ssl_context.h>

#include "protocol/frame_packet.h"
#include "tls_identity.h"
#include "transport/quic_listener.h"
#include "transport/stream_transport.h"

namespace {

int g_failures = 0;

void expect(bool ok, const char* what) {
  if (!ok) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

}  // namespace

int main() {
  using namespace uvcpp;

  const std::string dir = "/tmp/molink-quic-loopback";
  std::error_code ignored;
  std::filesystem::create_directories(dir, ignored);
  const molink::tls_identity identity =
      molink::load_or_create_tls_identity(dir, "molink-quic-loopback");
  if (!identity.ok()) {
    std::printf("SKIP: no TLS identity: %s\n", identity.error.c_str());
    return 77;
  }

  uvcpp_loop loop;
  expect(loop.init() == 0, "loop init");

  uvcpp_ssl_context client_tls(tls_mode::CLIENT);
  // PEER, not PEER_STRICT: the generated certificate carries a CN and no SAN,
  // so strict hostname checking would reject the 127.0.0.1 literal even though
  // the certificate itself is the one pinned here.
  client_tls.set_verify_mode(tls_verify_mode::PEER);
  expect(client_tls.load_ca_file(identity.cert_path),
         "client trusts the device certificate");
  client_tls.set_alpn_protos({"molink"});

  bool hello_seen = false;
  bool welcome_seen = false;
  bool binary_echo = false;
  bool client_failed = false;
  bool timed_out = false;
  std::shared_ptr<molink::stream_transport> server_link;
  std::shared_ptr<molink::stream_transport> client_link;

  // The shipping listener does the QUIC server half: one stream per
  // connection, admitted as a molink::stream_transport exactly as main() feeds
  // it into a session_controller.
  molink::quic_listener listener(
      &loop, identity.cert_path, identity.key_path,
      [&](std::shared_ptr<molink::stream_transport> link) {
        server_link = link;
        link->set_handlers(
            [&](const std::string& text) {
              hello_seen = text.find("\"hello\"") != std::string::npos;
              server_link->send_text("{\"type\":\"welcome\",\"protocol\":1}");
            },
            [&](const uint8_t* data, std::size_t len) {
              molink::protocol::packet packet;
              std::string error;
              if (molink::protocol::decode_packet(data, len, packet, error)) {
                server_link->send_binary(data, len);
              }
            },
            [&](int, const std::string&) {});
      });
  std::string listen_error;
  const bool listening = listener.start("127.0.0.1", 0, listen_error);
  if (!listening) {
    std::fprintf(stderr, "quic_listener.start failed: %s\n",
                 listen_error.c_str());
  }
  expect(listening, "QUIC listener accepts");

  // Close the endpoint while the loop is still running, then let it return;
  // the listener destructor must not have to stop a dead loop.
  auto finish = [&] {
    listener.stop();
    loop.stop();
  };

  uvcpp_quic_client client(&loop);
  client.set_ssl_context(&client_tls);
  client.set_alpn_protos({"molink"});
  const int port = listener.port();
  const int rc = client.connect("127.0.0.1", port, [&](int status) {
    if (status != 0) {
      std::fprintf(stderr, "client handshake failed (%d)\n", status);
      client_failed = true;
      finish();
      return;
    }
    uvcpp_quic_connection* connection = client.connection();
    if (connection == nullptr) {
      client_failed = true;
      finish();
      return;
    }
    const int64_t stream_id = connection->open_stream(true);
    if (stream_id < 0) {
      std::fprintf(stderr, "open_stream failed (%lld)\n",
                   static_cast<long long>(stream_id));
      client_failed = true;
      finish();
      return;
    }
    client_link = std::make_shared<molink::stream_transport>(
        [connection, stream_id](const uint8_t* data, std::size_t len) {
          return connection->write_stream(
                     stream_id, reinterpret_cast<const char*>(data), len,
                     false) == 0;
        },
        [connection](int, const std::string&) { connection->close(); });
    client_link->set_handlers(
        [&](const std::string& text) {
          welcome_seen = text.find("\"welcome\"") != std::string::npos;
          if (welcome_seen && binary_echo) finish();
        },
        [&](const uint8_t* data, std::size_t len) {
          molink::protocol::packet packet;
          std::string error;
          if (molink::protocol::decode_packet(data, len, packet, error)) {
            binary_echo =
                packet.type ==
                static_cast<uint8_t>(molink::protocol::packet_type::ping);
          }
          if (welcome_seen && binary_echo) finish();
        },
        [&](int, const std::string&) {});
    client_link->send_text("{\"type\":\"hello\",\"token\":\"test\"}");
    molink::protocol::packet ping;
    ping.type = static_cast<uint8_t>(molink::protocol::packet_type::ping);
    std::vector<uint8_t> bytes;
    std::string error;
    if (molink::protocol::encode_packet(ping, bytes, error)) {
      client_link->send_binary(bytes.data(), bytes.size());
    }
  });
  expect(rc == 0, "QUIC client starts connecting");
  if (rc == 0) {
    uvcpp_quic_connection* connection = client.connection();
    if (connection != nullptr) {
      uvcpp_quic_connection::callbacks callbacks;
      callbacks.on_read = [&](uvcpp_quic_connection&, int64_t,
                              const net_read_result& result) {
        if (client_link == nullptr) return;
        if (result.event == net_read_event::DATA) {
          client_link->feed(reinterpret_cast<const uint8_t*>(result.data),
                            result.size);
        } else if (result.event == net_read_event::PEER_CLOSED) {
          client_link->peer_closed("peer closed the stream");
        }
      };
      callbacks.on_close = [&](uvcpp_quic_connection&, int error_code) {
        if (client_link) {
          client_link->peer_closed("connection closed (" +
                                   std::to_string(error_code) + ")");
        }
      };
      connection->set_callbacks(callbacks);
    }
  }

  uvcpp_timer timer(&loop);
  timer.init();
  timer.start(
      [&](uvcpp_timer*) {
        timed_out = true;
        finish();
      },
      8000, 0);

  loop.run(UV_RUN_DEFAULT);

  expect(!client_failed, "QUIC handshake and stream setup succeed");
  expect(!timed_out, "the round-trip finishes before the timeout");
  expect(hello_seen, "the server received the JSON hello");
  expect(welcome_seen, "the client received the JSON welcome");
  expect(binary_echo, "a binary MOLINK packet round-trips");

  std::printf("quic loopback: %s\n", g_failures == 0 ? "PASS" : "FAIL");
  return g_failures == 0 ? 0 : 1;
}

#endif  // UVCPP_QUIC_ENABLE
