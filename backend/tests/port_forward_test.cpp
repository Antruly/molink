// TCP port forwarding: mapping persistence, a real local listener driven by a
// fake session bridge, byte counters, port release and the capability gate.
//
// No WebSocket and no libuvcpp: the fake bridge calls the manager/host entry
// points directly, which is exactly the seam the real session adapter uses.

#include <atomic>
#include <cerrno>
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
#include "net/socket_wait.h"
#include "portmap_api.h"
#include "protocol/frame_packet.h"
#include "transport/port_forward_bridge.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/resource.h>
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
    if (!molink::wait_readable(fd, static_cast<int>(left.count()))) return false;
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
    if (!molink::wait_readable(fd, 100)) continue;
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
    // Wait in short slices so stop() does not need to close the listening
    // socket under a blocked accept().
    while (!stopping_.load()) {
      if (!molink::wait_readable(listener_, 100)) continue;
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

/// A session transport with no socket: exactly the seam `connect_proxy` and
/// `direct_proxy` hand to `port_forward_bridge`. It records what the manager
/// puts on the wire and lets the test act as the remote host.
class test_session_transport final : public molink::session_transport {
 public:
  const char* name() const override { return "test"; }
  std::string peer_ip() const override { return "127.0.0.1"; }
  unsigned int peer_port() const override { return 1; }
  bool is_open() const override { return open_.load(); }

  bool send_text(const std::string& text) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      texts_.push_back(text);
    }
    condition_.notify_all();
    return true;
  }

  bool send_binary(const uint8_t* data, std::size_t len) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      binaries_.emplace_back(data, data + len);
    }
    condition_.notify_all();
    return true;
  }

  void close(int, const std::string&) override { open_.store(false); }

  void set_handlers(text_handler, binary_handler, close_handler) override {}

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

  std::string first_text_containing(const std::string& needle) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const std::string& text : texts_) {
      if (text.find(needle) != std::string::npos) return text;
    }
    return std::string();
  }

  bool wait_binary(std::vector<uint8_t>& out, int timeout_ms = 3000) {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool ready = condition_.wait_for(
        lock, std::chrono::milliseconds(timeout_ms),
        [&] { return !binaries_.empty(); });
    if (ready) {
      out = binaries_.front();
      binaries_.erase(binaries_.begin());
    }
    return ready;
  }

 private:
  std::atomic<bool> open_{true};
  std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<std::string> texts_;
  std::vector<std::vector<uint8_t>> binaries_;
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

void test_api_error_mapping() {
  std::printf("api vocabulary:\n");
  using molink::portmap_error;
  expect(molink::portmap_valid_port(0, true), "listen port 0 is allowed");
  expect(!molink::portmap_valid_port(0, false), "target port 0 is refused");
  expect(molink::portmap_valid_port(1, false), "port 1 is valid");
  expect(molink::portmap_valid_port(65535, true), "port 65535 is valid");
  expect(!molink::portmap_valid_port(65536, true), "port 65536 is refused");
  expect(!molink::portmap_valid_port(-1, true), "negative port is refused");

  expect(molink::portmap_valid_host("127.0.0.1"), "IPv4 host is valid");
  expect(molink::portmap_valid_host("example.lan"), "host name is valid");
  expect(!molink::portmap_valid_host(""), "empty host is refused");
  expect(!molink::portmap_valid_host("bad host"), "space in host is refused");

  expect(molink::portmap_error_from_manager("mapping_not_found") ==
             portmap_error::not_found,
         "manager not-found maps to not_found");
  expect(molink::portmap_error_from_manager("port_forward_disabled") ==
             portmap_error::port_forward_disabled,
         "capability refusal maps to port_forward_disabled");
  expect(molink::portmap_error_from_manager("cannot bind 127.0.0.1:80") ==
             portmap_error::port_in_use,
         "bind failure maps to port_in_use");
  expect(molink::portmap_error_from_manager("cannot write settings file") ==
             portmap_error::save_failed,
         "write failure maps to save_failed");
  expect(molink::portmap_error_from_manager("cannot bind 1.2.3.4:22") ==
             portmap_error::port_in_use,
         "unresolvable listen host is reported as port_in_use");
  expect(std::string(molink::portmap_error_code(
             portmap_error::port_forward_disabled)) ==
             "port_forward_disabled",
         "wire code keeps its documented spelling");

  std::size_t count = 0;
  const molink::portmap_preset* presets = molink::portmap_presets(count);
  expect(count == 6, "six quick presets are offered");
  const uint16_t expected_ports[] = {3389, 22, 3306, 80, 443, 445};
  for (std::size_t i = 0; i < count && i < 6; ++i) {
    expect(presets[i].port == expected_ports[i], "preset port is correct");
  }
  std::printf("  done\n");
}

/// Exercises exactly the manager calls behind POST/GET/PATCH/DELETE. The
/// listener is real (listen_port 0 lets the OS assign one) but no session is
/// attached: the API's CRUD does not need one.
void test_api_crud() {
  std::printf("api crud:\n");
  using molink::portmap_error;
  const std::filesystem::path directory = make_temp_dir("api-crud");
  const std::filesystem::path settings = directory / "settings";

  molink::port_forward_manager manager(
      molink::make_settings_port_forward_store(settings.string()),
      [] { return true; });
  molink::port_mapping mapping;
  mapping.listen_host = "127.0.0.1";
  mapping.listen_port = 0;
  mapping.target_host = "127.0.0.1";
  mapping.target_port = 22;
  std::string error;
  expect(manager.add(mapping, error), "POST adds a mapping");
  expect(mapping.id != 0, "POST assigns an id");
  expect(mapping.listening, "POST starts the listener");
  expect(mapping.listen_port != 0, "POST reports the OS-assigned port");
  const uint16_t assigned = mapping.listen_port;

  const std::vector<molink::port_mapping> listed = manager.mappings();
  expect(listed.size() == 1, "GET lists the new mapping");
  molink::port_mapping fetched;
  expect(manager.get(mapping.id, fetched), "GET finds the mapping by id");
  expect(fetched.listen_port == assigned, "GET reports the assigned port");
  expect(fetched.bytes_in == 0 && fetched.bytes_out == 0,
         "a new mapping starts at zero traffic");

  expect(manager.set_target(mapping.id, "127.0.0.1", 3389, error),
         "PATCH changes the target");
  expect(manager.get(mapping.id, fetched), "PATCH leaves the row in place");
  expect(fetched.target_port == 3389, "PATCH stores the new target port");
  expect(fetched.listening && fetched.listen_port == assigned,
         "a target edit keeps the listener and its port");

  expect(manager.set_enabled(mapping.id, false, error),
         "PATCH disables the mapping");
  expect(manager.get(mapping.id, fetched) && !fetched.listening,
         "a disabled mapping stops listening");
  expect(manager.set_enabled(mapping.id, true, error),
         "PATCH re-enables the mapping");
  expect(manager.get(mapping.id, fetched) && fetched.listening,
         "the listener comes back on the stored port");

  expect(manager.remove(mapping.id, error), "DELETE removes the mapping");
  expect(!manager.get(mapping.id, fetched), "the removed mapping is gone");
  expect(manager.mappings().empty(), "DELETE leaves no row");
  expect(!manager.remove(mapping.id, error) &&
             molink::portmap_error_from_manager(error) ==
                 portmap_error::not_found,
         "DELETE of a missing id reports not_found");

  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
  std::printf("  done\n");
}

/// The real proxy path: a mapping's local client is relayed through the
/// session-transport seam used by /ws/connect and /ws/direct. The test acts as
/// the remote host, so this covers forward-open, FORWARD_DATA in both
/// directions, and detach.
void test_session_bridge() {
  std::printf("session bridge:\n");
  const std::filesystem::path directory = make_temp_dir("bridge");
  const std::filesystem::path settings = directory / "settings";
  echo_server echo;
  std::string error;
  expect(echo.start(error), "echo target starts");

  auto manager = std::make_shared<molink::port_forward_manager>(
      molink::make_settings_port_forward_store(settings.string()),
      [] { return true; });
  molink::port_mapping mapping = make_mapping(echo.port(), 0);
  expect(manager->add(mapping, error), "bridge mapping listens");

  auto transport = std::make_shared<test_session_transport>();
  {
    molink::port_forward_bridge bridge(manager, transport);
    bridge.attach();
    expect(manager->linked(), "attach marks the session linked");

    test_socket client = connect_test_socket(mapping.listen_port);
    expect(client != kBadSocket, "local client reaches the listener");
    expect(transport->wait_text_contains("forward-open"),
           "forward-open goes over the session channel");
    const std::string open_text =
        transport->first_text_containing("forward-open");
    molink::port_forward_message open;
    expect(molink::decode_port_forward_message(open_text, open, error),
           "forward-open decodes");
    expect(open.type == "forward-open", "the message is forward-open");
    expect(open.host == "127.0.0.1" && open.port == echo.port(),
           "forward-open names the mapping's target");
    expect(open.stream != 0, "forward-open carries a stream id");

    expect(bridge.handle_text(molink::encode_forward_open_ok(open.stream)),
           "forward-open-ok is consumed by the bridge");
    expect(!bridge.handle_text("{\"type\":\"welcome\"}"),
           "ordinary host text still reaches the page");

    const uint8_t probe[] = {'b', 'r', 'i', 'd', 'g', 'e'};
    expect(send_all_test(client, probe, sizeof(probe)), "probe sent");
    std::vector<uint8_t> packet;
    expect(transport->wait_binary(packet),
           "client bytes become one FORWARD_DATA packet");
    molink::protocol::packet decoded;
    expect(molink::protocol::decode_packet(packet.data(), packet.size(),
                                           decoded, error),
           "FORWARD_DATA is a valid MOLK packet");
    expect(decoded.type == static_cast<uint8_t>(
                              molink::protocol::packet_type::forward_data),
           "the packet type is FORWARD_DATA");
    uint16_t stream = 0;
    const uint8_t* payload = nullptr;
    std::size_t payload_len = 0;
    expect(molink::decode_port_forward_payload(
               decoded.payload.data(), decoded.payload.size(), stream, payload,
               payload_len),
           "FORWARD_DATA payload decodes");
    expect(stream == open.stream, "stream id round-trips");
    expect(payload_len == sizeof(probe) &&
               std::memcmp(payload, probe, sizeof(probe)) == 0,
           "probe bytes round-trip");

    // Be the host: the same bytes echo back on the same stream.
    expect(bridge.handle_binary(packet.data(), packet.size()),
           "inbound FORWARD_DATA is consumed");
    uint8_t echoed[sizeof(probe)] = {};
    expect(recv_exact(client, echoed, sizeof(echoed)),
           "echoed bytes reach the local client");
    expect(std::memcmp(echoed, probe, sizeof(probe)) == 0,
           "the tunnel is byte-exact");

    molink::protocol::packet unrelated;
    std::vector<uint8_t> unrelated_bytes;
    expect(molink::protocol::encode_packet_into(
               unrelated_bytes,
               static_cast<uint8_t>(molink::protocol::packet_type::ping), 0, 0,
               0, 0, nullptr, 0, error),
           "an ordinary packet serialises");
    expect(!bridge.handle_binary(unrelated_bytes.data(),
                                 unrelated_bytes.size()),
           "non-forward packets still reach the page");

    bridge.detach();
    expect(!manager->linked(), "detach unlinks the session");
    shutdown_test_socket(client);
    close_test_socket(client);
    molink::port_mapping stats;
    wait_for([&] {
      return manager->get(mapping.id, stats) && stats.active_streams == 0;
    });
    expect(manager->get(mapping.id, stats) && stats.listening,
           "the listener survives detach");

    // No session means a new local connection is refused, but accepted.
    test_socket rejected = connect_test_socket(mapping.listen_port);
    expect(rejected != kBadSocket, "the listener still accepts after detach");
    if (rejected != kBadSocket) {
      expect(expect_closed(rejected), "a session-less connection is refused");
      close_test_socket(rejected);
    }
  }

  expect(manager->remove(mapping.id, error), "bridge mapping removed");
  echo.stop();
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
  std::printf("  done\n");
}

// Regression: select()/FD_SET aborts the whole process once the descriptor is
// >= FD_SETSIZE (1024 with glibc's fortify checks), which a long-running host
// reaches with one socket per forwarded connection. wait_writable() goes
// through poll()/WSAPoll() and must handle such descriptors normally.
void test_wait_writable_high_fd() {
#if defined(_WIN32)
  // Winsock descriptors are handles, not small integers, and wait_writable()
  // uses WSAPoll(): there is no FD_SETSIZE boundary to cross on Windows.
  std::printf("  wait_writable high-fd: skipped on Windows\n");
#else
  // 1100 descriptors are needed. Raise the soft limit towards the hard one
  // when the caller's is lower; skip if the environment still refuses.
  struct rlimit limit{};
  if (::getrlimit(RLIMIT_NOFILE, &limit) == 0 &&
      limit.rlim_cur < limit.rlim_max) {
    limit.rlim_cur = limit.rlim_max < 4096 ? limit.rlim_max : 4096;
    ::setrlimit(RLIMIT_NOFILE, &limit);
  }

  int pair[2] = {-1, -1};
  expect(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0,
         "socketpair for the high-fd check");
  if (pair[0] < 0) return;
  const int flags = ::fcntl(pair[1], F_GETFL, 0);
  if (flags >= 0) ::fcntl(pair[1], F_SETFL, flags | O_NONBLOCK);

  // dup() shares the same socket, so filling its send queue through pair[1]
  // also makes the high descriptor non-writable.
  std::vector<int> filler;
  filler.reserve(1100);
  int high = -1;
  while (filler.size() < 1100) {
    const int fd = ::dup(pair[1]);
    if (fd < 0) break;
    if (fd >= 1024 && high < 0) high = fd;
    filler.push_back(fd);
  }
  if (high < 1024) {
    std::printf("  wait_writable high-fd: skipped (%zu descriptors available)\n",
                filler.size());
    for (int fd : filler) ::close(fd);
    ::close(pair[0]);
    ::close(pair[1]);
    return;
  }

  // An idle stream socket is writable, even above FD_SETSIZE.
  expect(molink::wait_writable(high, 100),
         "wait_writable succeeds on fd >= 1024");

  char chunk[65536];
  std::memset(chunk, 'x', sizeof(chunk));
  bool full = false;
  for (int i = 0; i < 4096 && !full; ++i) {
    const ssize_t written = ::send(pair[1], chunk, sizeof(chunk), 0);
    if (written < 0) {
      full = errno == EAGAIN || errno == EWOULDBLOCK;
      break;
    }
    if (written == 0) break;
  }
  expect(full, "socketpair send queue filled");
  expect(!molink::wait_writable(high, 10),
         "wait_writable times out on fd >= 1024 without aborting");

  for (int fd : filler) ::close(fd);
  ::close(pair[0]);
  ::close(pair[1]);
#endif
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
  test_api_error_mapping();
  test_api_crud();
  test_session_bridge();
  test_capability_gate();
  test_bind_failure_rolls_back();
  test_host_connect_failure();
  test_echo_and_recovery();
  test_wait_writable_high_fd();
#if defined(_WIN32)
  WSACleanup();
#endif
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
