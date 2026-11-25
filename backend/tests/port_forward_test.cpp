// TCP port forwarding: mapping persistence, a real local listener driven by a
// fake session bridge, byte counters, port release and the capability gate.
//
// No WebSocket and no libuvcpp: the fake bridge calls the manager/host entry
// points directly, which is exactly the seam the real session adapter uses.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "port_forward.h"
#include "protocol/frame_packet.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

int checks = 0;
int failures = 0;

void expect(bool ok, const char* what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

#if defined(_WIN32)
using test_socket = SOCKET;
constexpr test_socket kBadSocket = INVALID_SOCKET;
#else
using test_socket = int;
constexpr test_socket kBadSocket = -1;
#endif

void close_test_socket(test_socket fd) {
#if defined(_WIN32)
  if (fd != kBadSocket) ::closesocket(fd);
#else
  if (fd >= 0) ::close(fd);
#endif
}

void shutdown_test_socket(test_socket fd) {
#if defined(_WIN32)
  if (fd != kBadSocket) ::shutdown(fd, SD_BOTH);
#else
  if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
#endif
}

void sleep_ms(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

template <typename Predicate>
bool wait_for(Predicate predicate, int timeout_ms = 3000) {
  const int steps = timeout_ms / 10;
  for (int i = 0; i < steps; ++i) {
    if (predicate()) return true;
    sleep_ms(10);
  }
  return predicate();
}

bool bind_test_socket(const char* host, uint16_t port, test_socket& out,
                      uint16_t& bound_port) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(host, service.c_str(), &hints, &result) != 0 ||
      result == nullptr) {
    return false;
  }
  test_socket fd = ::socket(result->ai_family, result->ai_socktype,
                            result->ai_protocol);
  if (fd == kBadSocket) {
    freeaddrinfo(result);
    return false;
  }
  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&one), sizeof(one));
  if (::bind(fd, result->ai_addr, static_cast<socklen_t>(result->ai_addrlen)) !=
      0) {
    close_test_socket(fd);
    freeaddrinfo(result);
    return false;
  }
  freeaddrinfo(result);
  sockaddr_in local{};
  socklen_t length = sizeof(local);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &length) != 0) {
    close_test_socket(fd);
    return false;
  }
  bound_port = ntohs(local.sin_port);
  out = fd;
  return true;
}

test_socket connect_test_socket(uint16_t port) {
  test_socket fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd == kBadSocket) return kBadSocket;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
      0) {
    close_test_socket(fd);
    return kBadSocket;
  }
  return fd;
}

bool send_all_test(test_socket fd, const uint8_t* data, std::size_t len) {
  std::size_t sent = 0;
  while (sent < len) {
    const int written =
        ::send(fd, reinterpret_cast<const char*>(data + sent),
               static_cast<int>(len - sent), 0);
    if (written > 0) {
      sent += static_cast<std::size_t>(written);
      continue;
    }
#if !defined(_WIN32)
    if (written < 0 && errno == EINTR) continue;
#endif
    return false;
  }
  return true;
}

bool recv_exact(test_socket fd, uint8_t* data, std::size_t len,
                int timeout_ms = 5000) {
  std::size_t received = 0;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  while (received < len) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) return false;
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(fd, &readable);
    timeval timeout{};
    timeout.tv_sec = static_cast<long>(left.count() / 1000);
    timeout.tv_usec = static_cast<long>((left.count() % 1000) * 1000);
#if defined(_WIN32)
    const int ready = ::select(0, &readable, nullptr, nullptr, &timeout);
#else
    const int ready = ::select(fd + 1, &readable, nullptr, nullptr, &timeout);
#endif
    if (ready < 0) {
#if !defined(_WIN32)
      if (errno == EINTR) continue;
#endif
      return false;
    }
    if (ready == 0) continue;
    const int got =
        ::recv(fd, reinterpret_cast<char*>(data + received),
               static_cast<int>(len - received), 0);
    if (got > 0) {
      received += static_cast<std::size_t>(got);
      continue;
    }
    return false;
  }
  return true;
}

/// True when the peer has closed. Accepts both a clean EOF and a reset: the
/// manager refuses an accepted connection while no session is attached.
bool expect_closed(test_socket fd, int timeout_ms = 3000) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(fd, &readable);
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000;
#if defined(_WIN32)
    const int ready = ::select(0, &readable, nullptr, nullptr, &timeout);
#else
    const int ready = ::select(fd + 1, &readable, nullptr, nullptr, &timeout);
#endif
    if (ready < 0) return true;
    if (ready == 0) continue;
    char buffer[16];
    const int got = ::recv(fd, buffer, sizeof(buffer), 0);
    if (got <= 0) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Echo target
// ---------------------------------------------------------------------------

class echo_server {
 public:
  ~echo_server() { stop(); }

  bool start(std::string& error) {
    if (!bind_test_socket("127.0.0.1", 0, listener_, bound_port_)) {
      error = "cannot bind echo listener";
      return false;
    }
    if (::listen(listener_, 16) != 0) {
      error = "cannot listen on echo socket";
      close_test_socket(listener_);
      listener_ = kBadSocket;
      return false;
    }
    thread_ = std::thread([this] { run(); });
    return true;
  }

  uint16_t port() const { return bound_port_; }

  void stop() {
    if (stopping_.exchange(true)) return;
    if (thread_.joinable()) thread_.join();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (test_socket fd : clients_) shutdown_test_socket(fd);
    }
    for (std::thread& worker : workers_) {
      if (worker.joinable()) worker.join();
    }
    workers_.clear();
  }

 private:
  void run() {
    // select() with a short timeout keeps stop() from needing to close the
    // listening socket under a blocked accept().
    while (!stopping_.load()) {
      fd_set readable;
      FD_ZERO(&readable);
      FD_SET(listener_, &readable);
      timeval timeout{};
      timeout.tv_sec = 0;
      timeout.tv_usec = 100000;
#if defined(_WIN32)
      const int ready = ::select(0, &readable, nullptr, nullptr, &timeout);
#else
      const int ready =
          ::select(listener_ + 1, &readable, nullptr, nullptr, &timeout);
#endif
      if (ready <= 0) continue;
      sockaddr_in peer{};
      socklen_t length = sizeof(peer);
      const test_socket client =
          ::accept(listener_, reinterpret_cast<sockaddr*>(&peer), &length);
      if (client == kBadSocket) {
        if (stopping_.load()) break;
        sleep_ms(10);
        continue;
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        clients_.push_back(client);
      }
      workers_.emplace_back([this, client] { serve(client); });
    }
    close_test_socket(listener_);
    listener_ = kBadSocket;
  }

  void serve(test_socket client) {
    std::vector<uint8_t> buffer(64 * 1024);
    for (;;) {
      const int got = ::recv(client, reinterpret_cast<char*>(buffer.data()),
                             static_cast<int>(buffer.size()), 0);
      if (got <= 0) break;
      if (!send_all_test(client, buffer.data(),
                         static_cast<std::size_t>(got))) {
        break;
      }
    }
    close_test_socket(client);
  }

  std::atomic<bool> stopping_{false};
  test_socket listener_ = kBadSocket;
  uint16_t bound_port_ = 0;
  std::thread thread_;
  std::mutex mutex_;
  std::vector<test_socket> clients_;
  std::vector<std::thread> workers_;
};

// ---------------------------------------------------------------------------
// Fake session bridge
// ---------------------------------------------------------------------------

struct bridge_state {
  std::atomic<bool> open{true};
  molink::port_forward_manager* manager = nullptr;
  molink::port_forward_host* host = nullptr;
};

class test_link final : public molink::port_forward_link {
 public:
  test_link(bridge_state* state, bool manager_side)
      : state_(state), manager_side_(manager_side) {}

  bool is_open() const override { return state_->open.load(); }

  bool send_text(const std::string& text) override {
    if (!state_->open.load()) return false;
    if (manager_side_) {
      if (state_->host == nullptr) return false;
      state_->host->on_control(text);
    } else {
      if (state_->manager == nullptr) return false;
      state_->manager->on_control(text);
    }
    return true;
  }

  bool send_data(uint16_t stream, const uint8_t* data,
                 std::size_t len) override {
    if (!state_->open.load()) return false;
    if (manager_side_) {
      if (state_->host == nullptr) return false;
      state_->host->on_data(stream, data, len);
    } else {
      if (state_->manager == nullptr) return false;
      state_->manager->on_data(stream, data, len);
    }
    return true;
  }

 private:
  bridge_state* state_;
  bool manager_side_;
};

class capture_link final : public molink::port_forward_link {
 public:
  bool is_open() const override { return true; }

  bool send_text(const std::string& text) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      texts_.push_back(text);
    }
    condition_.notify_all();
    return true;
  }

  bool send_data(uint16_t, const uint8_t*, std::size_t) override {
    return true;
  }

  bool wait_text_contains(const std::string& needle, int timeout_ms = 3000) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                               [&] {
                                 for (const std::string& text : texts_) {
                                   if (text.find(needle) != std::string::npos) {
                                     return true;
                                   }
                                 }
                                 return false;
                               });
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<std::string> texts_;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::filesystem::path make_temp_dir(const char* name) {
  static std::atomic<int> counter{0};
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch();
  const auto nanos =
      std::chrono::duration_cast<std::chrono::nanoseconds>(stamp).count();
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      (std::string("molink-port-forward-") + name + "-" +
       std::to_string(static_cast<long long>(nanos)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::error_code error;
  std::filesystem::create_directories(path, error);
  return path;
}

molink::port_mapping make_mapping(uint16_t target_port, uint16_t listen_port) {
  molink::port_mapping mapping;
  mapping.listen_host = "127.0.0.1";
  mapping.listen_port = listen_port;
  mapping.target_host = "127.0.0.1";
  mapping.target_port = target_port;
  mapping.enabled = true;
  return mapping;
}

void test_message_codec() {
  std::printf("message codec:\n");
  molink::port_forward_message message;
  std::string error;

  const std::string open =
      molink::encode_forward_open(7, "example host", 3389);
  expect(molink::decode_port_forward_message(open, message, error),
         "forward-open round-trips");
  expect(message.type == "forward-open", "forward-open type");
  expect(message.stream == 7, "forward-open stream");
  expect(message.host == "example host", "forward-open host");
  expect(message.port == 3389, "forward-open port");

  const std::string ok = molink::encode_forward_open_ok(65535);
  expect(molink::decode_port_forward_message(ok, message, error),
         "forward-open-ok round-trips");
  expect(message.stream == 65535, "forward-open-ok max stream");

  const std::string fail =
      molink::encode_forward_open_fail(9, "port_forward_disabled");
  expect(molink::decode_port_forward_message(fail, message, error),
         "forward-open-fail round-trips");
  expect(message.error == "port_forward_disabled", "fail error preserved");

  const std::string close = molink::encode_forward_close(3);
  expect(molink::decode_port_forward_message(close, message, error),
         "forward-close round-trips");

  expect(!molink::decode_port_forward_message("{\"type\":\"forward-open\"}",
                                              message, error),
         "missing stream is rejected");
  expect(!molink::decode_port_forward_message(
             "{\"type\":\"forward-open\",\"stream\":0,\"host\":\"x\","
             "\"port\":1}",
             message, error),
         "stream 0 is rejected");
  expect(!molink::decode_port_forward_message("not json", message, error),
         "non-JSON is rejected");

  const std::vector<uint8_t> payload =
      molink::encode_port_forward_payload(0x1234, nullptr, 0);
  expect(payload.size() == 2 && payload[0] == 0x12 && payload[1] == 0x34,
         "stream id is network order");
  uint16_t stream = 0;
  const uint8_t* body = nullptr;
  std::size_t body_len = 0;
  expect(molink::decode_port_forward_payload(payload.data(), payload.size(),
                                             stream, body, body_len) &&
             stream == 0x1234 && body_len == 0,
         "payload round-trips");
  const uint8_t truncated[1] = {0};
  expect(!molink::decode_port_forward_payload(truncated, 1, stream, body,
                                              body_len),
         "short payload is rejected");

  // The session adapter wraps these bytes in a MOLK packet whose type byte is
  // the independent forward_data type, not file_data.
  const uint8_t raw[] = {'h', 'i'};
  const std::vector<uint8_t> framed_payload =
      molink::encode_port_forward_payload(0x0042, raw, sizeof(raw));
  std::vector<uint8_t> packet;
  error.clear();
  expect(molink::protocol::encode_packet_into(
             packet,
             static_cast<uint8_t>(molink::protocol::packet_type::forward_data),
             0, 0, 0, 0, framed_payload.data(), framed_payload.size(), error),
         "forward payload is wrapped in a MOLK packet");
  molink::protocol::packet decoded;
  error.clear();
  expect(molink::protocol::decode_packet(packet.data(), packet.size(),
                                         decoded, error),
         "MOLK packet decodes");
  expect(decoded.type ==
             static_cast<uint8_t>(molink::protocol::packet_type::forward_data),
         "MOLK type byte is forward_data");
  expect(molink::protocol::packet_stream_of(packet.data(), packet.size()) ==
             molink::protocol::packet_stream::file,
         "forward data rides the reliable file channel");
  uint16_t packet_stream = 0;
  const uint8_t* packet_body = nullptr;
  std::size_t packet_body_len = 0;
  expect(molink::decode_port_forward_payload(
             decoded.payload.data(), decoded.payload.size(), packet_stream,
             packet_body, packet_body_len) &&
             packet_stream == 0x0042 && packet_body_len == sizeof(raw) &&
             std::memcmp(packet_body, raw, sizeof(raw)) == 0,
         "stream id and bytes survive the MOLK round-trip");
  std::printf("  done\n");
}

void test_persistence() {
  std::printf("persistence:\n");
  const std::filesystem::path directory = make_temp_dir("persist");
  const std::filesystem::path settings = directory / "settings";
  {
    std::ofstream seed(settings, std::ios::binary | std::ios::trunc);
    seed << "# leave me alone\n";
    seed << "allow_remote_access=1\n";
    seed << "device_name=test-box\n";
  }

  uint64_t kept_id = 0;
  {
    molink::port_forward_manager manager(
        molink::make_settings_port_forward_store(settings.string()));
    molink::port_mapping first;
    first.listen_port = 0;
    first.target_host = "10.0.0.5";
    first.target_port = 22;
    first.enabled = false;  // configuration only: no listener in this test
    std::string error;
    expect(manager.add(first, error), "add persists the first mapping");
    kept_id = first.id;
    expect(first.id != 0, "add assigns an id");

    molink::port_mapping second = first;
    second.id = 0;
    second.target_host = "example.lan";
    second.target_port = 3306;
    expect(manager.add(second, error), "add persists the second mapping");
    const uint64_t removed_id = second.id;

    first.target_port = 2222;
    expect(manager.update(first, error), "update persists a changed mapping");
    expect(manager.remove(removed_id, error), "remove persists the deletion");
  }

  {
    std::ifstream input(settings, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
    expect(content.find("allow_remote_access=1") != std::string::npos,
           "unrelated setting survives a mapping write");
    expect(content.find("# leave me alone") != std::string::npos,
           "comment survives a mapping write");
    const std::size_t first = content.find("port_mapping=");
    expect(first != std::string::npos, "mapping line was written");
    expect(content.find("port_mapping=", first + 1) == std::string::npos,
           "exactly one mapping line remains");
  }

  {
    molink::port_forward_manager reloaded(
        molink::make_settings_port_forward_store(settings.string()));
    const std::vector<molink::port_mapping> mappings = reloaded.mappings();
    expect(mappings.size() == 1, "one mapping reloads");
    if (mappings.size() == 1) {
      expect(mappings[0].id == kept_id, "mapping id survives a reload");
      expect(mappings[0].target_host == "10.0.0.5",
             "target host survives a reload");
      expect(mappings[0].target_port == 2222,
             "updated target port survives a reload");
      expect(!mappings[0].enabled, "enabled flag survives a reload");
    }
  }
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
  std::printf("  done\n");
}

void test_capability_gate() {
  std::printf("capability gate:\n");
  const std::filesystem::path directory = make_temp_dir("gate");
  const std::filesystem::path settings = directory / "settings";
  molink::port_forward_manager manager(
      molink::make_settings_port_forward_store(settings.string()),
      [] { return false; });

  molink::port_mapping enabled = make_mapping(22, 0);
  std::string error;
  expect(!manager.add(enabled, error), "enabled mapping is refused when off");
  expect(error == "port_forward_disabled", "refusal names the capability");
  expect(manager.mappings().empty(), "refused mapping is not stored");

  molink::port_mapping disabled = make_mapping(22, 0);
  disabled.enabled = false;
  expect(manager.add(disabled, error),
         "a disabled mapping may still be configured while off");
  expect(!disabled.listening, "a disabled mapping does not listen");

  // Target side answers forward-open with the documented error.
  auto capture = std::make_shared<capture_link>();
  molink::port_forward_host host(capture, [] { return false; });
  host.on_control(molink::encode_forward_open(4, "127.0.0.1", 22));
  expect(capture->wait_text_contains("forward-open-fail"),
         "host answers a disabled open");
  expect(capture->wait_text_contains("port_forward_disabled"),
         "host names port_forward_disabled");
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
  std::printf("  done\n");
}

void test_bind_failure_rolls_back() {
  std::printf("bind failure:\n");
  test_socket occupied = kBadSocket;
  uint16_t occupied_port = 0;
  expect(bind_test_socket("127.0.0.1", 0, occupied, occupied_port),
         "can occupy a port");
  expect(::listen(occupied, 8) == 0, "occupied port is listening");

  const std::filesystem::path directory = make_temp_dir("bind-fail");
  const std::filesystem::path settings = directory / "settings";
  {
    molink::port_forward_manager manager(
        molink::make_settings_port_forward_store(settings.string()),
        [] { return true; });
    molink::port_mapping mapping = make_mapping(22, occupied_port);
    std::string error;
    expect(!manager.add(mapping, error),
           "mapping on an occupied port is refused");
    expect(!error.empty(), "bind failure carries an error");
    expect(manager.mappings().empty(),
           "failed mapping is not left in the manager");
  }
  {
    std::ifstream input(settings, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
    expect(content.find("port_mapping=") == std::string::npos,
           "failed mapping is not persisted");
  }
  close_test_socket(occupied);
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
  std::printf("  done\n");
}

void test_host_connect_failure() {
  std::printf("target connect failure:\n");
  // Grab a port that is free right now, then let it go: connecting to it
  // fails quickly with no server.
  test_socket probe = kBadSocket;
  uint16_t closed_port = 0;
  expect(bind_test_socket("127.0.0.1", 0, probe, closed_port),
         "can reserve a probe port");
  close_test_socket(probe);

  auto capture = std::make_shared<capture_link>();
  molink::port_forward_host host(capture, [] { return true; });
  host.on_control(
      molink::encode_forward_open(5, "127.0.0.1", closed_port));
  expect(capture->wait_text_contains("\"stream\":5"),
         "failure names the stream");
  expect(capture->wait_text_contains("forward-open-fail"),
         "unreachable target is reported as forward-open-fail");
  std::printf("  done\n");
}

void test_echo_and_recovery() {
  std::printf("listener, echo, counters and reconnect:\n");
  echo_server echo;
  std::string error;
  expect(echo.start(error), "echo target starts");

  const std::filesystem::path directory = make_temp_dir("echo");
  const std::filesystem::path settings = directory / "settings";
  molink::port_forward_manager manager(
      molink::make_settings_port_forward_store(settings.string()),
      [] { return true; });

  molink::port_mapping mapping = make_mapping(echo.port(), 0);
  expect(manager.add(mapping, error), "mapping added and listener starts");
  expect(mapping.listening, "mapping reports listening");
  expect(mapping.listen_port != 0, "system-assigned port is backfilled");

  // The TCP listener is bound before the link exists; a connection that
  // arrives while detached must be refused instead of queued.
  const uint16_t local_port = mapping.listen_port;
  {
    test_socket rejected = connect_test_socket(local_port);
    expect(rejected != kBadSocket, "detached listener still accepts TCP");
    if (rejected != kBadSocket) {
      expect(expect_closed(rejected), "detached mapping refuses the client");
      close_test_socket(rejected);
    }
  }

  bridge_state bridge;
  bridge.manager = &manager;
  auto manager_link = std::make_shared<test_link>(&bridge, true);
  auto host_link = std::make_shared<test_link>(&bridge, false);
  auto host = std::make_unique<molink::port_forward_host>(
      host_link, [] { return true; });
  bridge.host = host.get();
  manager.attach_link(manager_link);
  expect(manager.linked(), "manager reports the attached session");

  const std::vector<uint8_t> payload = [] {
    std::vector<uint8_t> bytes(256 * 1024);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      bytes[i] = static_cast<uint8_t>((i * 31u + 7u) & 0xff);
    }
    return bytes;
  }();

  {
    test_socket client = connect_test_socket(local_port);
    expect(client != kBadSocket, "client connects to the local listener");
    if (client != kBadSocket) {
      expect(send_all_test(client, payload.data(), payload.size()),
             "client sends the payload");
      std::vector<uint8_t> echoed(payload.size());
      expect(recv_exact(client, echoed.data(), echoed.size()),
             "echoed bytes arrive through the tunnel");
      expect(echoed == payload, "echoed bytes match exactly");
      shutdown_test_socket(client);
      close_test_socket(client);
    }
  }

  molink::port_mapping stats;
  expect(wait_for([&] {
           return manager.get(mapping.id, stats) && stats.active_streams == 0;
         }),
         "stream is reclaimed after the client closes");
  expect(manager.get(mapping.id, stats), "mapping still present");
  expect(stats.bytes_in == payload.size(), "bytes_in counts client->target");
  expect(stats.bytes_out == payload.size(), "bytes_out counts target->client");

  // Session drop: streams close and the listener stays bound. A local client
  // is refused; after a new session attaches, the same mapping works again.
  bridge.open.store(false);
  manager.detach_link();
  wait_for([&] {
    return manager.get(mapping.id, stats) && stats.active_streams == 0;
  });
  host->stop();
  host.reset();
  bridge.host = nullptr;
  {
    test_socket rejected = connect_test_socket(local_port);
    expect(rejected != kBadSocket, "listener survives a session drop");
    if (rejected != kBadSocket) {
      expect(expect_closed(rejected), "no session means the client is refused");
      close_test_socket(rejected);
    }
  }

  bridge.open.store(true);
  host = std::make_unique<molink::port_forward_host>(
      host_link, [] { return true; });
  bridge.host = host.get();
  manager.attach_link(manager_link);
  {
    test_socket client = connect_test_socket(local_port);
    expect(client != kBadSocket, "client connects after reconnect");
    if (client != kBadSocket) {
      const uint8_t probe[] = {'r', 'e', 'c', 'o', 'v', 'e', 'r'};
      expect(send_all_test(client, probe, sizeof(probe)), "probe sent");
      uint8_t echoed[sizeof(probe)] = {};
      expect(recv_exact(client, echoed, sizeof(echoed)), "probe echoed");
      expect(std::memcmp(echoed, probe, sizeof(probe)) == 0,
             "same mapping works after reconnect");
      shutdown_test_socket(client);
      close_test_socket(client);
    }
  }
  wait_for([&] {
    return manager.get(mapping.id, stats) && stats.active_streams == 0;
  });

  // Port release: remove the mapping and bind its port again. The manager's
  // listener joins before remove() returns.
  expect(manager.remove(mapping.id, error), "mapping removed");
  test_socket rebound = kBadSocket;
  uint16_t rebound_port = 0;
  expect(bind_test_socket("127.0.0.1", local_port, rebound, rebound_port),
         "removed mapping releases its port");
  if (rebound != kBadSocket) close_test_socket(rebound);

  // The same explicit port can be handed to a new mapping immediately.
  molink::port_mapping again = make_mapping(echo.port(), local_port);
  expect(manager.add(again, error), "port can be reused by a new mapping");
  expect(again.listening, "reused mapping listens");
  expect(manager.remove(again.id, error), "cleanup removal");

  echo.stop();
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
  std::printf("  done\n");
}

}  // namespace

int main() {
#if defined(_WIN32)
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    std::fprintf(stderr, "WSAStartup failed\n");
    return 1;
  }
#endif
  test_message_codec();
  test_persistence();
  test_capability_gate();
  test_bind_failure_rolls_back();
  test_host_connect_failure();
  test_echo_and_recovery();
#if defined(_WIN32)
  WSACleanup();
#endif
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
