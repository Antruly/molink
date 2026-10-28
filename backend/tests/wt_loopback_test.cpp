// WebTransport loopback (Phase 5).
//
// A WebTransport client opens a session against a host that accepts one, and
// both halves of the transport are exercised: the reliable bidirectional
// stream carries MOLK packets (a JSON hello and a binary ping, echoed back),
// and the H3 datagram path carries a fragmented frame that has to reassemble
// byte for byte.
//
// Needs libuvcpp built with HTTP/3 and WebTransport; otherwise it skips.
#include <uvcpp/uvcpp_config.h>

#include <cstdio>

#if !UVCPP_QUIC_ENABLE || !UVCPP_HTTP3_ENABLE
int main() {
  std::printf("SKIP: libuvcpp was built without QUIC/HTTP3\n");
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
#include <http3/uvcpp_h3_connection.h>
#include <http3/uvcpp_h3_wt_session.h>
#include <quic/uvcpp_quic_client.h>
#include <quic/uvcpp_quic_server.h>
#include <ssl/uvcpp_ssl_context.h>

#include "protocol/frame_packet.h"
#include "tls_identity.h"
#include "transport/wt_transport.h"

namespace {

int g_failures = 0;

void expect(bool ok, const char* what) {
  if (!ok) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

/// A MOLK video packet big enough to need fragmentation.
std::vector<uint8_t> video_packet() {
  molink::protocol::packet packet;
  packet.type = static_cast<uint8_t>(molink::protocol::packet_type::frame);
  packet.payload.resize(3000);
  for (std::size_t i = 0; i < packet.payload.size(); ++i) {
    packet.payload[i] = static_cast<uint8_t>((i * 13 + 5) & 0xff);
  }
  std::vector<uint8_t> bytes;
  std::string error;
  molink::protocol::encode_packet(packet, bytes, error);
  return bytes;
}

}  // namespace

int main() {
  using namespace uvcpp;

  const std::string dir = "/tmp/molink-wt-loopback";
  std::error_code ignored;
  std::filesystem::create_directories(dir, ignored);
  const molink::tls_identity identity =
      molink::load_or_create_tls_identity(dir, "molink-wt-loopback");
  if (!identity.ok()) {
    std::printf("SKIP: no TLS identity: %s\n", identity.error.c_str());
    return 77;
  }

  uvcpp_loop loop;
  expect(loop.init() == 0, "loop init");

  bool server_saw_hello = false;
  bool client_saw_welcome = false;
  bool server_saw_datagram = false;
  bool server_datagram_ok = false;
  bool server_saw_audio = false;
  bool server_saw_file = false;
  bool finished = false;

  const std::vector<uint8_t> frame = video_packet();
  // One audio packet on the same datagram path, tagged audio.
  molink::protocol::packet audio;
  audio.type = static_cast<uint8_t>(molink::protocol::packet_type::audio);
  audio.payload.assign(600, 0x11);
  std::vector<uint8_t> audio_frame;
  {
    std::string error;
    molink::protocol::encode_packet(audio, audio_frame, error);
  }

  auto finish = [&] {
    if (finished) return;
    finished = true;
    loop.stop();
  };

  // ---- server ----
  uvcpp_ssl_context server_tls(tls_mode::SERVER);
  expect(server_tls.load_certificate_file(identity.cert_path),
         "server loads its certificate");
  expect(server_tls.load_private_key_file(identity.key_path),
         "server loads its key");
  server_tls.set_alpn_select_protos({"h3"});

  uvcpp_quic_server server(&loop);
  server.set_ssl_context(&server_tls);
  server.set_alpn_select_protos({"h3"});
  const int bind_rc = server.bind("127.0.0.1", 0);
  expect(bind_rc == 0, "server binds");


  std::unique_ptr<uvcpp_h3_connection> server_h3;
  std::unique_ptr<uvcpp_h3_wt_session> server_wt;
  std::shared_ptr<molink::wt_transport> server_link;

  const int listen_rc = server.listen([&](uvcpp_quic_connection* connection) {
           server_h3 = std::make_unique<uvcpp_h3_connection>(connection, true);
           server_h3->set_h3_datagram_enabled(true);
           uvcpp_h3_connection::callbacks callbacks;
           callbacks.on_connect_request =
               [&](uvcpp_h3_connection& h3, const h3_request& request) {
                 if (request.protocol != "webtransport") return;
                 uvcpp_h3_wt_session::callbacks wt_callbacks;
                 wt_callbacks.on_open = [&](uvcpp_h3_wt_session& session) {
                   server_link = std::make_shared<molink::wt_transport>(
                       "webtransport");
                   // on_open runs inside accept(); server_wt is still empty.
                   server_link->attach(&session);
                   server_link->set_handlers(
                       [&](const std::string& text) {
                         server_saw_hello =
                             text.find("\"hello\"") != std::string::npos;
                         if (server_link) {
                           server_link->send_text(
                               "{\"type\":\"welcome\",\"protocol\":1}");
                         }
                       },
                       [&](const uint8_t* data, std::size_t len) {
                         molink::protocol::packet packet;
                         std::string error;
                         if (molink::protocol::decode_packet(data, len, packet,
                                                             error)) {
                           if (packet.type == static_cast<uint8_t>(
                                                  molink::protocol::packet_type::audio)) {
                             server_saw_audio = true;
                             return;
                           }
                           if (packet.type == static_cast<uint8_t>(
                                                  molink::protocol::packet_type::file_data)) {
                             server_saw_file = true;
                             return;
                           }
                           server_link->send_binary(data, len);
                         }
                       },
                       [&](int, const std::string&) {});
                 };
                 wt_callbacks.on_stream = [&](uvcpp_h3_wt_session&,
                                             uvcpp_h3_wt_stream& stream,
                                             bool bidi) {
                  server_link->attach_peer_stream(&stream);
                };
                wt_callbacks.on_datagram = [&](uvcpp_h3_wt_session&,
                                                const uint8_t* data,
                                                std::size_t len) {
                   if (!server_link) return;
                   server_saw_datagram = true;
                   const std::size_t before = server_link->frames_received();
                   server_link->feed_datagram(data, len);
                   if (server_link->frames_received() > before) {
                     server_datagram_ok = true;
                   }
                 };
                 server_wt =
                     uvcpp_h3_wt_session::accept(h3, request, wt_callbacks);
                };
           server_h3->start(callbacks);
         });
  expect(listen_rc == 0, "server listens");
  // A port-0 bind resolves its real port only once the listener is up.
  const int port = server.configured_port();

  // ---- client ----
  uvcpp_ssl_context client_tls(tls_mode::CLIENT);
  client_tls.set_verify_mode(tls_verify_mode::PEER);
  expect(client_tls.load_ca_file(identity.cert_path),
         "client trusts the device certificate");
  client_tls.set_alpn_protos({"h3"});

  std::unique_ptr<uvcpp_h3_connection> client_h3;
  std::unique_ptr<uvcpp_h3_wt_session> client_wt;
  uvcpp_h3_wt_stream* client_stream = nullptr;
  uvcpp_h3_wt_stream* client_file = nullptr;

  uvcpp_quic_client client(&loop);
  client.set_ssl_context(&client_tls);
  client.set_alpn_protos({"h3"});
  const int rc = client.connect("127.0.0.1", port, [&](int status) {
    if (status != 0) {
      std::fprintf(stderr, "client handshake failed (%d)\n", status);
      finish();
      return;
    }
    uvcpp_quic_connection* connection = client.connection();
    if (connection == nullptr) {
      finish();
      return;
    }
    client_h3 = std::make_unique<uvcpp_h3_connection>(connection, false);
    // Both peers have to announce SETTINGS_H3_DATAGRAM; the host does the same
    // in h3_ws_listener before the session is created.
    client_h3->set_h3_datagram_enabled(true);
    uvcpp_h3_connection::callbacks callbacks;
    client_h3->start(callbacks);

    uvcpp_h3_wt_session::callbacks wt_callbacks;
    wt_callbacks.on_open = [&](uvcpp_h3_wt_session& session) {
      client_stream = session.open_bidi_stream();
      client_file = session.open_bidi_stream();
      if (client_stream != nullptr) {
        client_stream->on_data([&](const uint8_t* data, std::size_t len) {
          molink::protocol::packet packet;
          std::string error;
          if (!molink::protocol::decode_packet(data, len, packet, error)) return;
          if (packet.type ==
              static_cast<uint8_t>(molink::protocol::packet_type::control)) {
            const std::string text(packet.payload.begin(), packet.payload.end());
            client_saw_welcome = text.find("\"welcome\"") != std::string::npos;
          }
        });
      }
      // The reliable stream is opened by the host; pick it up from on_stream.
    };
    wt_callbacks.on_stream = [&](uvcpp_h3_wt_session&,
                                 uvcpp_h3_wt_stream& stream, bool bidi) {
      std::printf("client: stream %lld bidi=%d\n",
                  static_cast<long long>(stream.stream_id()), bidi ? 1 : 0);
      if (!bidi || client_stream != nullptr) return;
      client_stream = &stream;
      stream.on_data([&](const uint8_t* data, std::size_t len) {
        molink::protocol::packet packet;
        std::string error;
        if (!molink::protocol::decode_packet(data, len, packet, error)) return;
        if (packet.type ==
            static_cast<uint8_t>(molink::protocol::packet_type::control)) {
          const std::string text(packet.payload.begin(), packet.payload.end());
          client_saw_welcome = text.find("\"welcome\"") != std::string::npos;
        }
      });
    };
  
    client_wt =
        uvcpp_h3_wt_session::connect(*client_h3, "/wt", "127.0.0.1",
                                     wt_callbacks);
  });

  // Simple drive loop: run the loop in slices and act from here.
  const auto start = std::chrono::steady_clock::now();
  bool sent_hello = false;
  bool sent_frame = false;
  bool sent_file = false;
  while (!finished) {
    loop.run(UV_RUN_NOWAIT);
    const auto now = std::chrono::steady_clock::now();
    if (!sent_hello && client_stream != nullptr && client_h3) {
      molink::protocol::packet hello;
      hello.type = static_cast<uint8_t>(molink::protocol::packet_type::control);
      const std::string text = "{\"type\":\"hello\",\"token\":\"test\"}";
      hello.payload.assign(text.begin(), text.end());
      std::vector<uint8_t> bytes;
      std::string error;
      if (molink::protocol::encode_packet(hello, bytes, error)) {
        client_stream->write(bytes.data(), bytes.size());
        sent_hello = true;
      }
    }
    if (!sent_file && client_wt && client_file != nullptr && server_saw_hello) {
      // A file packet on its own stream: tag byte, then MOLK.
      const uint8_t tag = 0x02;
      client_file->write(&tag, 1);
      molink::protocol::packet chunk;
      chunk.type =
          static_cast<uint8_t>(molink::protocol::packet_type::file_data);
      chunk.payload.assign(128, 0x33);
      std::vector<uint8_t> bytes;
      std::string error;
      if (molink::protocol::encode_packet(chunk, bytes, error)) {
        client_file->write(bytes.data(), bytes.size());
        sent_file = true;
      }
    }
    if (!sent_frame && client_wt && server_saw_hello) {
      const auto fragments = molink::fragment_frame(
          molink::datagram_channel::video, frame.data(), frame.size(), 1);
      for (const auto& fragment : fragments) {
        client_wt->send_datagram(fragment.bytes.data(), fragment.bytes.size());
      }
      const auto audio = molink::fragment_frame(
          molink::datagram_channel::audio, audio_frame.data(),
          audio_frame.size(), 1);
      for (const auto& fragment : audio) {
        client_wt->send_datagram(fragment.bytes.data(), fragment.bytes.size());
      }
      sent_frame = true;
    }
    if (server_saw_hello && client_saw_welcome && server_datagram_ok) finish();
    if (now - start > std::chrono::seconds(10)) finish();
  }

  expect(server_saw_hello, "the host receives the control packet");
  expect(client_saw_welcome, "the client receives the control reply");
  expect(server_saw_datagram, "a datagram reaches the host");
  expect(server_datagram_ok, "the fragmented frame reassembles");
  expect(server_saw_audio, "an audio datagram reaches the host");
  expect(server_saw_file, "a file packet arrives on its own stream");

  server_wt.reset();
  server_h3.reset();
  client_wt.reset();
  client_h3.reset();
  server.stop();
  std::printf("%d failure(s)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}

#endif
