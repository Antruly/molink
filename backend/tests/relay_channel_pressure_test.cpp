// Relay transport under a stalled receiver.
//
// libuvcpp's WebSocket client queues frames without a bound and reports every
// accepted frame as sent, so without byte accounting in relay_transport a slow
// receiver just grows that queue and no backpressure ever reaches the
// scheduler. This test stalls a real loopback receiver, pushes file traffic
// through a real relay_transport, and checks the three things Phase 1 promises:
//
//   * the queued file bytes stay bounded instead of growing with the push,
//   * control keeps queueing while the file channel is backpressured,
//   * every file message the transport accepted is still delivered, in order,
//     once the receiver resumes.
//
// The payload is deliberately incompressible: uvcpp negotiates
// permessage-deflate by default, and a filler of one repeated byte compresses
// a 64 KiB frame to a few hundred bytes, which fits in the kernel socket
// buffers and never applies any pressure at all.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <uvcpp/uvcpp_config.h>

#include "protocol/frame_packet.h"
#include "transport/relay_transport.h"

#if UVCPP_WEB_ENABLE && UVCPP_WEB_WS_ENABLE
#include <web/uvcpp_ws_server.h>
#endif

namespace {

namespace protocol = molink::protocol;

int g_failures = 0;

void expect(bool ok, const char* what) {
  if (!ok) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

using namespace std::chrono_literals;

/// Deterministic, incompressible-looking filler derived from the transfer id.
/// Both sides regenerate it, so corruption and reordering are both visible.
void fill_payload(uint8_t* out, std::size_t len, uint32_t id) {
  uint64_t state = 0x9e3779b97f4a7c15ull ^ (static_cast<uint64_t>(id) << 32);
  for (std::size_t i = 0; i < len; ++i) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    out[i] = static_cast<uint8_t>(state >> 24);
  }
}

/// The payload of one file_data packet: 16-byte transfer header, then filler.
std::vector<uint8_t> file_payload(uint32_t id, std::size_t filler) {
  std::vector<uint8_t> payload(16 + filler);
  std::memcpy(payload.data(), &id, sizeof(id));
  const uint64_t offset = static_cast<uint64_t>(id) * filler;
  std::memcpy(payload.data() + 8, &offset, sizeof(offset));
  fill_payload(payload.data() + 16, filler, id);
  return payload;
}

std::vector<uint8_t> file_packet(uint32_t id, std::size_t filler) {
  const std::vector<uint8_t> payload = file_payload(id, filler);
  std::vector<uint8_t> out;
  std::string err;
  protocol::encode_packet_into(
      out, static_cast<uint8_t>(protocol::packet_type::file_data), 0, 0, 0, 0,
      payload.data(), payload.size(), err);
  return out;
}

struct received {
  std::mutex mutex;
  std::vector<uint32_t> ids;
  std::size_t corrupt = 0;
  std::size_t text_messages = 0;

  std::size_t count() {
    std::lock_guard<std::mutex> lock(mutex);
    return ids.size();
  }
};

#if UVCPP_WEB_ENABLE && UVCPP_WEB_WS_ENABLE

/// Accepts one session, records what arrives, and stops reading entirely while
/// @p paused is set. The kernel send buffer then fills, which is the slow
/// receiver the transport has to notice.
void run_server(std::promise<int> port_promise, std::atomic<bool>& running,
                std::atomic<bool>& paused, received& sink) {
  uvcpp::uvcpp_ws_server server;
  int bound = -1;
  for (int candidate = 39200; candidate < 39300; ++candidate) {
    if (server.bind("127.0.0.1", candidate) == 0) {
      bound = candidate;
      break;
    }
  }
  if (bound < 0 || server.listen() != 0) {
    port_promise.set_value(-1);
    return;
  }
  server.on_connection([&sink](uvcpp::uvcpp_ws_connection* connection) {
    connection->on_binary([&sink](const uint8_t* data, std::size_t len) {
      protocol::packet packet;
      std::string err;
      if (!protocol::decode_packet(data, len, packet, err) ||
          packet.type !=
              static_cast<uint8_t>(protocol::packet_type::file_data) ||
          packet.payload.size() <= 16) {
        std::lock_guard<std::mutex> lock(sink.mutex);
        ++sink.corrupt;
        return;
      }
      uint32_t id = 0;
      std::memcpy(&id, packet.payload.data(), sizeof(id));
      const std::vector<uint8_t> expected =
          file_payload(id, packet.payload.size() - 16);
      std::lock_guard<std::mutex> lock(sink.mutex);
      if (packet.payload != expected) {
        ++sink.corrupt;
        return;
      }
      sink.ids.push_back(id);
    });
    connection->on_text([&sink](const std::string&) {
      std::lock_guard<std::mutex> lock(sink.mutex);
      ++sink.text_messages;
    });
  });
  port_promise.set_value(bound);

  while (running) {
    if (paused.load()) {
      std::this_thread::sleep_for(1ms);
      continue;
    }
    server.run(UV_RUN_NOWAIT);
  }
  server.stop();
}

#endif

}  // namespace

int main() {
#if !(UVCPP_WEB_ENABLE && UVCPP_WEB_WS_ENABLE)
  std::printf("SKIP: libuvcpp was built without WEB or WEB WS\n");
  return 77;
#else
  received sink;
  std::atomic<bool> running{true};
  std::atomic<bool> paused{false};
  std::promise<int> port_promise;
  std::future<int> port_future = port_promise.get_future();
  std::thread server_thread(
      [&] { run_server(std::move(port_promise), running, paused, sink); });
  const int port = port_future.get();
  if (port < 0) {
    std::printf("SKIP: no free loopback port for the receiver\n");
    running = false;
    server_thread.join();
    return 77;
  }

  molink::relay_transport relay(
      "ws://127.0.0.1:" + std::to_string(port) + "/ws/session", std::string(),
      nullptr, /*relay_protocol=*/false);
  relay.start();
  const auto ready_deadline = std::chrono::steady_clock::now() + 10s;
  while (!relay.is_open() && std::chrono::steady_clock::now() < ready_deadline) {
    std::this_thread::sleep_for(1ms);
  }
  expect(relay.is_open(), "relay transport connects to the loopback receiver");

  // Stall the receiver, then push file packets until the transport refuses.
  paused = true;
  std::this_thread::sleep_for(50ms);

  constexpr std::size_t kFiller = 64 * 1024;
  constexpr std::size_t kPushLimit = 4096;
  std::size_t accepted = 0;
  std::size_t accepted_bytes = 0;
  while (accepted < kPushLimit) {
    const std::vector<uint8_t> packet =
        file_packet(static_cast<uint32_t>(accepted), kFiller);
    if (!relay.send_binary(molink::transport_channel::file, packet.data(),
                           packet.size())) {
      break;
    }
    accepted_bytes += packet.size();
    ++accepted;
    // Give the transport thread room to drain what it can, so the rejection
    // comes from the receiver not moving rather than from a burst.
    if ((accepted % 16) == 0) std::this_thread::sleep_for(1ms);
  }

  expect(accepted > 0, "the file channel accepts traffic before the stall");
  expect(accepted < kPushLimit,
         "a stalled receiver eventually refuses file bytes");
  expect(accepted_bytes < 32u * 1024u * 1024u,
         "queued file bytes stay bounded instead of growing with the push");
  expect(relay.send_text(molink::transport_channel::control,
                         "{\"type\":\"ping\"}"),
         "control still queues while the file channel is backpressured");

  // Resume the receiver: everything the transport accepted has to arrive.
  paused = false;
  const auto drain_deadline = std::chrono::steady_clock::now() + 30s;
  while (sink.count() < accepted &&
         std::chrono::steady_clock::now() < drain_deadline) {
    std::this_thread::sleep_for(2ms);
  }

  std::printf("stalled receiver: accepted %zu file messages (%.1f MiB)\n",
              accepted, static_cast<double>(accepted_bytes) / (1024.0 * 1024.0));
  {
    std::lock_guard<std::mutex> lock(sink.mutex);
    expect(sink.ids.size() == accepted,
           "every accepted file message is delivered once the receiver moves");
    expect(sink.corrupt == 0, "delivered packets are intact");
    for (std::size_t i = 0; i < sink.ids.size(); ++i) {
      if (sink.ids[i] != i) {
        expect(false, "file messages arrive in the order they were accepted");
        break;
      }
    }
    expect(sink.text_messages == 1,
           "the control message queued during backpressure is delivered");
  }

  relay.close(molink::transport_close::normal, "test done");
  running = false;
  server_thread.join();
  return g_failures == 0 ? 0 : 1;
#endif
}
