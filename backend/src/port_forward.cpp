#include "port_forward.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include "diag_log.h"

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
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace molink {
namespace {

// ---------------------------------------------------------------------------
// A flat JSON object is all the control protocol needs. Keeping the parser
// local means this module (and its tests) do not depend on libuvcpp/nlohmann.
// ---------------------------------------------------------------------------

void json_skip_ws(const std::string& text, std::size_t& i) {
  while (i < text.size() &&
         (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' ||
          text[i] == '\n')) {
    ++i;
  }
}

void json_append_utf8(std::string& out, unsigned int code) {
  if (code <= 0x7f) {
    out.push_back(static_cast<char>(code));
  } else if (code <= 0x7ff) {
    out.push_back(static_cast<char>(0xc0 | (code >> 6)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
  } else if (code <= 0xffff) {
    out.push_back(static_cast<char>(0xe0 | (code >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
  } else {
    out.push_back(static_cast<char>(0xf0 | (code >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
  }
}

bool json_hex4(const std::string& text, std::size_t at, unsigned int& value) {
  if (at + 4 > text.size()) return false;
  value = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    const char c = text[at + i];
    value <<= 4;
    if (c >= '0' && c <= '9') {
      value |= static_cast<unsigned int>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      value |= static_cast<unsigned int>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      value |= static_cast<unsigned int>(c - 'A' + 10);
    } else {
      return false;
    }
  }
  return true;
}

bool json_parse_string(const std::string& text, std::size_t& i,
                       std::string& out) {
  if (i >= text.size() || text[i] != '"') return false;
  ++i;
  out.clear();
  while (i < text.size()) {
    const char c = text[i++];
    if (c == '"') return true;
    if (c != '\\') {
      out.push_back(c);
      continue;
    }
    if (i >= text.size()) return false;
    const char escape = text[i++];
    switch (escape) {
      case '"':
      case '\\':
      case '/':
        out.push_back(escape);
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case 'u': {
        unsigned int code = 0;
        if (!json_hex4(text, i, code)) return false;
        i += 4;
        // A surrogate pair arrives as two \u escapes; fold it back into one
        // scalar so a non-BMP hostname/error keeps its bytes.
        if (code >= 0xd800 && code <= 0xdbff && i + 6 <= text.size() &&
            text[i] == '\\' && text[i + 1] == 'u') {
          unsigned int low = 0;
          if (json_hex4(text, i + 2, low) && low >= 0xdc00 && low <= 0xdfff) {
            code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
            i += 6;
          }
        }
        json_append_utf8(out, code);
        break;
      }
      default:
        return false;
    }
  }
  return false;
}

bool json_skip_value(const std::string& text, std::size_t& i);

bool json_skip_container(const std::string& text, std::size_t& i, char open,
                         char close) {
  if (i >= text.size() || text[i] != open) return false;
  ++i;
  json_skip_ws(text, i);
  if (i < text.size() && text[i] == close) {
    ++i;
    return true;
  }
  while (i < text.size()) {
    if (open == '{') {
      std::string key;
      if (!json_parse_string(text, i, key)) return false;
      json_skip_ws(text, i);
      if (i >= text.size() || text[i] != ':') return false;
      ++i;
    }
    if (!json_skip_value(text, i)) return false;
    json_skip_ws(text, i);
    if (i < text.size() && text[i] == ',') {
      ++i;
      json_skip_ws(text, i);
      continue;
    }
    if (i < text.size() && text[i] == close) {
      ++i;
      return true;
    }
    return false;
  }
  return false;
}

bool json_skip_value(const std::string& text, std::size_t& i) {
  json_skip_ws(text, i);
  if (i >= text.size()) return false;
  if (text[i] == '"') {
    std::string ignored;
    return json_parse_string(text, i, ignored);
  }
  if (text[i] == '{') return json_skip_container(text, i, '{', '}');
  if (text[i] == '[') return json_skip_container(text, i, '[', ']');
  // Number, true, false or null: consume the token without interpreting it.
  const std::size_t start = i;
  while (i < text.size() && text[i] != ',' && text[i] != '}' &&
         text[i] != ']' && text[i] != ' ' && text[i] != '\t' &&
         text[i] != '\r' && text[i] != '\n') {
    ++i;
  }
  return i > start;
}

struct flat_json {
  std::map<std::string, std::string> strings;
  std::map<std::string, double> numbers;
};

bool json_parse_flat(const std::string& text, flat_json& out,
                     std::string& error) {
  std::size_t i = 0;
  json_skip_ws(text, i);
  if (i >= text.size() || text[i] != '{') {
    error = "control message must be a JSON object";
    return false;
  }
  ++i;
  json_skip_ws(text, i);
  if (i < text.size() && text[i] == '}') return true;
  while (i < text.size()) {
    std::string key;
    if (!json_parse_string(text, i, key)) {
      error = "bad JSON key";
      return false;
    }
    json_skip_ws(text, i);
    if (i >= text.size() || text[i] != ':') {
      error = "bad JSON object";
      return false;
    }
    ++i;
    json_skip_ws(text, i);
    if (i >= text.size()) {
      error = "truncated JSON value";
      return false;
    }
    if (text[i] == '"') {
      std::string value;
      if (!json_parse_string(text, i, value)) {
        error = "bad JSON string value";
        return false;
      }
      out.strings[key] = std::move(value);
    } else if (text[i] == '-' || (text[i] >= '0' && text[i] <= '9')) {
      const char* begin = text.c_str() + i;
      char* end = nullptr;
      const double value = std::strtod(begin, &end);
      if (end == begin) {
        error = "bad JSON number";
        return false;
      }
      i = static_cast<std::size_t>(end - text.c_str());
      out.numbers[key] = value;
    } else if (!json_skip_value(text, i)) {
      error = "bad JSON value";
      return false;
    }
    json_skip_ws(text, i);
    if (i < text.size() && text[i] == ',') {
      ++i;
      json_skip_ws(text, i);
      continue;
    }
    if (i < text.size() && text[i] == '}') return true;
    error = "bad JSON object separator";
    return false;
  }
  error = "truncated JSON object";
  return false;
}

std::string json_escape(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (const char c : value) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                        static_cast<unsigned char>(c));
          out += buffer;
        } else {
          out.push_back(c);
        }
        break;
    }
  }
  return out;
}

/// Parses a stream id from a JSON number. Returns false for 0, fractions and
/// values above 65535.
bool json_stream(const flat_json& json, uint16_t& stream) {
  const auto it = json.numbers.find("stream");
  if (it == json.numbers.end()) return false;
  const double value = it->second;
  if (!(value >= 1.0 && value <= 65535.0)) return false;
  if (static_cast<double>(static_cast<uint32_t>(value)) != value) return false;
  stream = static_cast<uint16_t>(value);
  return true;
}

// ---------------------------------------------------------------------------
// Settings persistence
// ---------------------------------------------------------------------------

bool starts_with(const std::string& text, const char* prefix) {
  const std::size_t n = std::strlen(prefix);
  return text.size() >= n && text.compare(0, n, prefix) == 0;
}

std::string trim_left(const std::string& text) {
  std::size_t i = 0;
  while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
  return text.substr(i);
}

std::string field_escape(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    switch (c) {
      case '%':
        out += "%25";
        break;
      case '|':
        out += "%7C";
        break;
      case '\n':
        out += "%0A";
        break;
      case '\r':
        out += "%0D";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

bool field_unescape(const std::string& value, std::string& out) {
  out.clear();
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '%') {
      out.push_back(value[i]);
      continue;
    }
    if (i + 2 >= value.size()) return false;
    const auto hex = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    const int hi = hex(value[i + 1]);
    const int lo = hex(value[i + 2]);
    if (hi < 0 || lo < 0) return false;
    out.push_back(static_cast<char>((hi << 4) | lo));
    i += 2;
  }
  return true;
}

std::string mapping_line(const port_mapping& mapping) {
  std::ostringstream out;
  out << "port_mapping=" << mapping.id << '|' << (mapping.enabled ? 1 : 0)
      << '|' << field_escape(mapping.listen_host) << '|' << mapping.listen_port
      << '|' << field_escape(mapping.target_host) << '|'
      << mapping.target_port;
  return out.str();
}

bool parse_mapping_line(const std::string& line, port_mapping& mapping) {
  if (!starts_with(line, "port_mapping=")) return false;
  const std::string body = line.substr(std::strlen("port_mapping="));
  std::vector<std::string> fields;
  std::string current;
  for (const char c : body) {
    if (c == '|') {
      fields.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  fields.push_back(current);
  if (fields.size() != 6) return false;
  try {
    port_mapping parsed;
    parsed.id = std::stoull(fields[0]);
    parsed.enabled = std::stoi(fields[1]) != 0;
    if (!field_unescape(fields[2], parsed.listen_host)) return false;
    const int listen_port = std::stoi(fields[3]);
    if (listen_port < 0 || listen_port > 65535) return false;
    parsed.listen_port = static_cast<uint16_t>(listen_port);
    if (!field_unescape(fields[4], parsed.target_host)) return false;
    const int target_port = std::stoi(fields[5]);
    if (target_port < 1 || target_port > 65535) return false;
    parsed.target_port = static_cast<uint16_t>(target_port);
    if (parsed.id == 0) return false;
    if (parsed.target_host.empty()) return false;
    if (parsed.listen_host.empty()) parsed.listen_host = "127.0.0.1";
    mapping = std::move(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

class settings_port_forward_store final : public port_forward_store {
 public:
  explicit settings_port_forward_store(std::string path)
      : path_(std::move(path)) {}

  bool load(std::vector<port_mapping>& mappings,
            std::string& error) override {
    mappings.clear();
    error.clear();
    if (path_.empty()) return true;
    std::ifstream input(path_, std::ios::binary);
    if (!input) return true;
    std::string line;
    while (std::getline(input, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      port_mapping mapping;
      if (parse_mapping_line(trim_left(line), mapping)) {
        mappings.push_back(std::move(mapping));
      }
    }
    return true;
  }

  bool save(const std::vector<port_mapping>& mappings,
            std::string& error) override {
    error.clear();
    if (path_.empty()) {
      error = "settings path is empty";
      return false;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path path(path_);
    if (path.has_parent_path()) {
      fs::create_directories(path.parent_path(), ec);
    }
    std::vector<std::string> preserved;
    {
      std::ifstream input(path_, std::ios::binary);
      std::string line;
      while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!starts_with(trim_left(line), "port_mapping=")) {
          preserved.push_back(line);
        }
      }
    }
    // A distinct temp name from settings.tmp: save_settings() may run on
    // another thread, and sharing the temp file would interleave two writers.
    const fs::path tmp = fs::path(path_ + ".portmap.tmp");
    std::ofstream output(tmp, std::ios::binary | std::ios::trunc);
    if (!output) {
      error = "cannot open settings file for writing";
      return false;
    }
    for (const std::string& line : preserved) output << line << '\n';
    for (const port_mapping& mapping : mappings) {
      output << mapping_line(mapping) << '\n';
    }
    output.close();
    if (!output) {
      error = "cannot write settings file";
      return false;
    }
#if !defined(_WIN32)
    ::chmod(tmp.c_str(), 0600);
#endif
    fs::rename(tmp, path, ec);
    if (ec) {
      // Windows rename does not replace an existing file.
      fs::remove(path, ec);
      fs::rename(tmp, path, ec);
    }
    if (ec) {
      error = "cannot replace settings file";
      return false;
    }
#if !defined(_WIN32)
    ::chmod(path_.c_str(), 0600);
#endif
    return true;
  }

 private:
  std::string path_;
};

// ---------------------------------------------------------------------------
// Sockets
// ---------------------------------------------------------------------------

#if defined(_WIN32)
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

bool socket_system_ready() {
#if defined(_WIN32)
  static std::once_flag once;
  static bool ready = false;
  std::call_once(once, [] {
    WSADATA data{};
    ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
  });
  return ready;
#else
  return true;
#endif
}

int socket_error() {
#if defined(_WIN32)
  return WSAGetLastError();
#else
  return errno;
#endif
}

bool error_would_block(int error) {
#if defined(_WIN32)
  return error == WSAEWOULDBLOCK;
#else
  return error == EAGAIN || error == EWOULDBLOCK ||
         error == EINPROGRESS;
#endif
}

bool error_in_progress(int error) {
#if defined(_WIN32)
  return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
  return error == EINPROGRESS;
#endif
}

bool error_interrupted(int error) {
#if defined(_WIN32)
  return error == WSAEINTR;
#else
  return error == EINTR;
#endif
}

void close_socket(socket_t fd) {
#if defined(_WIN32)
  if (fd != kInvalidSocket) ::closesocket(fd);
#else
  if (fd >= 0) ::close(fd);
#endif
}

void shutdown_socket(socket_t fd) {
#if defined(_WIN32)
  if (fd != kInvalidSocket) ::shutdown(fd, SD_BOTH);
#else
  if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
#endif
}

bool set_nonblocking(socket_t fd, bool enabled) {
#if defined(_WIN32)
  u_long value = enabled ? 1 : 0;
  return ::ioctlsocket(fd, FIONBIO, &value) == 0;
#else
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  const int next = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  return ::fcntl(fd, F_SETFL, next) == 0;
#endif
}

void set_nodelay(socket_t fd) {
  const int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&one), sizeof(one));
}

std::uint16_t sockaddr_port(const sockaddr_storage& address) {
  if (address.ss_family == AF_INET) {
    return ntohs(reinterpret_cast<const sockaddr_in&>(address).sin_port);
  }
  if (address.ss_family == AF_INET6) {
    return ntohs(reinterpret_cast<const sockaddr_in6&>(address).sin6_port);
  }
  return 0;
}

struct addrinfo_deleter {
  void operator()(addrinfo* value) const {
    if (value != nullptr) freeaddrinfo(value);
  }
};
using addrinfo_ptr = std::unique_ptr<addrinfo, addrinfo_deleter>;

bool resolve(const std::string& host, uint16_t port, bool passive,
             addrinfo_ptr& out, std::string& error) {
  if (!socket_system_ready()) {
    error = "socket initialization failed";
    return false;
  }
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  if (passive) hints.ai_flags = AI_PASSIVE;
  const std::string service = std::to_string(port);
  addrinfo* result = nullptr;
  const int status =
      ::getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(),
                    &hints, &result);
  if (status != 0 || result == nullptr) {
    error = "cannot resolve " + (host.empty() ? std::string("*") : host);
    return false;
  }
  out.reset(result);
  return true;
}

/// Binds and listens. The returned socket is non-blocking; @p bound_port is the
/// port the OS actually assigned (needed when @p port was 0).
bool bind_listen(const std::string& host, uint16_t port, socket_t& out,
                 uint16_t& bound_port, std::string& error) {
  addrinfo_ptr addresses;
  if (!resolve(host, port, true, addresses, error)) return false;
  for (addrinfo* address = addresses.get(); address != nullptr;
       address = address->ai_next) {
    socket_t fd = ::socket(address->ai_family, address->ai_socktype,
                           address->ai_protocol);
    if (fd == kInvalidSocket) continue;
    // Allow a removed mapping's port to be bound again immediately. On Windows
    // this also lets a listener come back while old accepted sockets sit in
    // TIME_WAIT, which is what "remove then re-add" needs.
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&one), sizeof(one));
    if (::bind(fd, address->ai_addr,
               static_cast<socklen_t>(address->ai_addrlen)) != 0) {
      close_socket(fd);
      continue;
    }
    if (::listen(fd, 128) != 0) {
      close_socket(fd);
      continue;
    }
    if (!set_nonblocking(fd, true)) {
      close_socket(fd);
      continue;
    }
    sockaddr_storage local{};
    socklen_t local_len = sizeof(local);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &local_len) ==
        0) {
      bound_port = sockaddr_port(local);
    } else {
      bound_port = port;
    }
    set_nodelay(fd);
    out = fd;
    return true;
  }
  error = "cannot bind " + (host.empty() ? std::string("*") : host) + ":" +
          std::to_string(port);
  return false;
}

/// Blocking-connect emulation with a bounded wait. @p cancelled is checked
/// between 100 ms slices so a session close never stalls the host loop for the
/// full timeout.
socket_t connect_target(const std::string& host, uint16_t port,
                        const std::atomic<bool>& cancelled,
                        std::string& error) {
  addrinfo_ptr addresses;
  if (!resolve(host, port, false, addresses, error)) return kInvalidSocket;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  for (addrinfo* address = addresses.get(); address != nullptr;
       address = address->ai_next) {
    if (cancelled.load()) {
      error = "cancelled";
      return kInvalidSocket;
    }
    socket_t fd = ::socket(address->ai_family, address->ai_socktype,
                           address->ai_protocol);
    if (fd == kInvalidSocket) continue;
    set_nonblocking(fd, true);
    const int status =
        ::connect(fd, address->ai_addr,
                  static_cast<socklen_t>(address->ai_addrlen));
    if (status != 0 && !error_in_progress(socket_error())) {
      close_socket(fd);
      continue;
    }
    bool connected = status == 0;
    while (!connected) {
      if (cancelled.load()) {
        close_socket(fd);
        error = "cancelled";
        return kInvalidSocket;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        close_socket(fd);
        error = "connect_timeout";
        return kInvalidSocket;
      }
      fd_set writable;
      fd_set failed;
      FD_ZERO(&writable);
      FD_ZERO(&failed);
      FD_SET(fd, &writable);
      FD_SET(fd, &failed);
      timeval timeout{};
      timeout.tv_sec = 0;
      timeout.tv_usec = 100000;
#if defined(_WIN32)
      const int ready = ::select(0, nullptr, &writable, &failed, &timeout);
#else
      const int ready = ::select(fd + 1, nullptr, &writable, &failed, &timeout);
#endif
      if (ready < 0) {
        if (error_interrupted(socket_error())) continue;
        close_socket(fd);
        error = "connect_failed";
        return kInvalidSocket;
      }
      if (ready == 0) continue;
      int socket_err = 0;
#if defined(_WIN32)
      int length = sizeof(socket_err);
      ::getsockopt(fd, SOL_SOCKET, SO_ERROR,
                   reinterpret_cast<char*>(&socket_err), &length);
#else
      socklen_t length = sizeof(socket_err);
      ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_err, &length);
#endif
      if (socket_err != 0) {
        close_socket(fd);
        error = "connect_failed";
        // Only a connection-level failure on this address; try the next one.
        connected = false;
        fd = kInvalidSocket;
        break;
      }
      connected = true;
    }
    if (!connected) continue;
    set_nonblocking(fd, false);
    set_nodelay(fd);
    return fd;
  }
  if (error.empty()) error = "connect_failed";
  return kInvalidSocket;
}

bool send_all(socket_t fd, const std::uint8_t* data, std::size_t len) {
  std::size_t sent = 0;
  while (sent < len) {
    const std::size_t chunk = (std::min)(len - sent, std::size_t{1} << 20);
    const int written =
        ::send(fd, reinterpret_cast<const char*>(data + sent),
               static_cast<int>(chunk), 0);
    if (written > 0) {
      sent += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && error_interrupted(socket_error())) continue;
    return false;
  }
  return true;
}

constexpr std::size_t kRelayChunkBytes = 64u * 1024u;
constexpr int kOpenTimeoutMs = 10000;
/// Per manager/host ceiling. A local (or remote) process must not be able to
/// turn one mapping into unbounded socket/thread growth.
constexpr std::size_t kMaxForwardStreams = 512;

void log_error(uint64_t id, const std::string& error) {
  diag().log(diag_level::warn, "portmap.error", std::string(),
             {{"id", std::to_string(id)}, {"err", error}});
}

void log_open(uint64_t id, uint16_t stream, const std::string& host,
              uint16_t port) {
  diag().log(diag_level::info, "portmap.open", std::string(),
             {{"id", std::to_string(id)},
              {"stream", std::to_string(stream)},
              {"target", host + ":" + std::to_string(port)}});
}

void log_close(uint64_t id, uint64_t bytes_in, uint64_t bytes_out,
               const std::string& reason) {
  diag().log(diag_level::info, "portmap.close", std::string(),
             {{"id", std::to_string(id)},
              {"bytes_in", std::to_string(bytes_in)},
              {"bytes_out", std::to_string(bytes_out)},
              {"reason", reason}});
}

/// The fd half of a relay endpoint. `write_mutex` is owned by the caller and
/// held for the entire send; the worker closes the socket only while holding
/// it, so a send can never race with close. request_shutdown() does not take
/// that mutex (otherwise it could not unblock a sender whose buffer is full).
struct socket_state {
  std::atomic<socket_t> fd{kInvalidSocket};
  std::mutex mutex;
  bool shutdown_done = false;
  bool closed = false;

  void request_shutdown() {
    std::lock_guard<std::mutex> lock(mutex);
    if (shutdown_done || closed) return;
    shutdown_done = true;
    shutdown_socket(fd.load());
  }

  /// Caller holds write_mutex.
  void close_owned() {
    std::lock_guard<std::mutex> lock(mutex);
    if (closed) return;
    closed = true;
    close_socket(fd.exchange(kInvalidSocket));
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// Wire codec
// ---------------------------------------------------------------------------

bool is_port_forward_message_type(const std::string& type) {
  return type == "forward-open" || type == "forward-open-ok" ||
         type == "forward-open-fail" || type == "forward-close";
}

bool decode_port_forward_message(const std::string& text,
                                 port_forward_message& out,
                                 std::string& error) {
  flat_json json;
  if (!json_parse_flat(text, json, error)) return false;
  const auto type = json.strings.find("type");
  if (type == json.strings.end() || !is_port_forward_message_type(type->second)) {
    error = "not a forward control message";
    return false;
  }
  out = port_forward_message();
  out.type = type->second;
  if (!json_stream(json, out.stream)) {
    error = "missing or invalid stream id";
    return false;
  }
  if (out.type == "forward-open") {
    const auto host = json.strings.find("host");
    if (host == json.strings.end() || host->second.empty()) {
      error = "forward-open is missing host";
      return false;
    }
    out.host = host->second;
    const auto port = json.numbers.find("port");
    if (port == json.numbers.end() || !(port->second >= 1.0 &&
                                        port->second <= 65535.0) ||
        static_cast<double>(static_cast<uint32_t>(port->second)) !=
            port->second) {
      error = "forward-open is missing a valid port";
      return false;
    }
    out.port = static_cast<uint16_t>(port->second);
  } else if (out.type == "forward-open-fail") {
    const auto description = json.strings.find("error");
    if (description != json.strings.end()) out.error = description->second;
  }
  return true;
}

std::string encode_forward_open(uint16_t stream, const std::string& host,
                                uint16_t port) {
  return "{\"type\":\"forward-open\",\"stream\":" + std::to_string(stream) +
         ",\"host\":\"" + json_escape(host) + "\",\"port\":" +
         std::to_string(port) + "}";
}

std::string encode_forward_open_ok(uint16_t stream) {
  return "{\"type\":\"forward-open-ok\",\"stream\":" +
         std::to_string(stream) + "}";
}

std::string encode_forward_open_fail(uint16_t stream,
                                     const std::string& error) {
  return "{\"type\":\"forward-open-fail\",\"stream\":" +
         std::to_string(stream) + ",\"error\":\"" + json_escape(error) + "\"}";
}

std::string encode_forward_close(uint16_t stream) {
  return "{\"type\":\"forward-close\",\"stream\":" + std::to_string(stream) +
         "}";
}

std::vector<uint8_t> encode_port_forward_payload(uint16_t stream,
                                                 const uint8_t* data,
                                                 std::size_t len) {
  std::vector<uint8_t> payload(2 + len);
  payload[0] = static_cast<uint8_t>((stream >> 8) & 0xff);
  payload[1] = static_cast<uint8_t>(stream & 0xff);
  if (len != 0 && data != nullptr) {
    std::memcpy(payload.data() + 2, data, len);
  }
  return payload;
}

bool decode_port_forward_payload(const uint8_t* data, std::size_t len,
                                 uint16_t& stream, const uint8_t*& payload,
                                 std::size_t& payload_len) {
  if (data == nullptr || len < 2) return false;
  stream = static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) |
                                 static_cast<uint16_t>(data[1]));
  payload = data + 2;
  payload_len = len - 2;
  return stream != 0;
}

std::unique_ptr<port_forward_store> make_settings_port_forward_store(
    std::string settings_path) {
  return std::make_unique<settings_port_forward_store>(
      std::move(settings_path));
}

// ---------------------------------------------------------------------------
// Controller-side manager
// ---------------------------------------------------------------------------

struct port_forward_manager::impl {
  using enabled_fn = port_forward_manager::enabled_fn;

  struct stream {
    uint16_t id = 0;
    uint64_t mapping_id = 0;
    std::shared_ptr<port_forward_link> link;
    socket_state socket;
    std::mutex write_mutex;

    std::mutex state_mutex;
    std::condition_variable state_cv;
    bool open_ok = false;
    bool open_failed = false;
    std::atomic<bool> closed{false};
    std::string error;
    std::string close_reason;

    uint64_t bytes_in = 0;
    uint64_t bytes_out = 0;
  };

  struct listener {
    impl* owner = nullptr;
    uint64_t mapping_id = 0;
    std::string host;
    uint16_t requested_port = 0;
    socket_t fd = kInvalidSocket;
    uint16_t bound_port = 0;
    std::atomic<bool> stopping{false};
    std::thread thread;

    listener(impl* owner_value, uint64_t id, std::string host_value,
             uint16_t port)
        : owner(owner_value),
          mapping_id(id),
          host(std::move(host_value)),
          requested_port(port) {}

    ~listener() { stop(); }

    bool start(std::string& error) {
      if (!bind_listen(host, requested_port, fd, bound_port, error)) {
        return false;
      }
      thread = std::thread([this] { run(); });
      return true;
    }

    void stop() {
      stopping.store(true);
      if (thread.joinable()) thread.join();
    }

    void run() {
      while (!stopping.load()) {
        sockaddr_storage address{};
        socklen_t length = sizeof(address);
        const socket_t client =
            ::accept(fd, reinterpret_cast<sockaddr*>(&address), &length);
        if (client == kInvalidSocket) {
          const int error = socket_error();
          if (error_would_block(error)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
          }
          if (error_interrupted(error)) continue;
          break;
        }
        set_nodelay(client);
        owner->on_accept(mapping_id, client);
      }
      close_socket(fd);
      fd = kInvalidSocket;
    }
  };

  struct entry {
    port_mapping mapping;
    std::shared_ptr<listener> listener_state;
  };

  impl(std::unique_ptr<port_forward_store> store_value, enabled_fn enabled_value)
      : store(std::move(store_value)), enabled(std::move(enabled_value)) {}

  ~impl() {
    std::vector<std::shared_ptr<listener>> listeners;
    std::vector<std::shared_ptr<stream>> live;
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopping = true;
      for (auto& item : entries) {
        if (item.second.listener_state) {
          listeners.push_back(std::move(item.second.listener_state));
        }
      }
      entries.clear();
      for (auto& item : streams) live.push_back(item.second);
      if (link) link->mark_closed();
      link.reset();
    }
    for (auto& listener : listeners) listener->stop();
    for (auto& stream : live) close_stream(stream, "shutdown");
    std::unique_lock<std::mutex> lock(mutex);
    streams_cv.wait(lock, [this] { return streams.empty(); });
  }

  bool allowed() const { return enabled && enabled(); }

  void on_accept(uint64_t mapping_id, socket_t client) {
    std::shared_ptr<port_forward_link> active_link;
    std::shared_ptr<stream> created;
    uint64_t target_id = mapping_id;
    std::string target_host;
    uint16_t target_port = 0;
    std::string error;
    // Read the settings-backed switch before taking the manager lock: it is
    // owned by auth_state and must not be called under our mutex.
    const bool capability = allowed();
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto entry_it = entries.find(mapping_id);
      if (entry_it == entries.end() || stopping) {
        error = "mapping_removed";
      } else if (!entry_it->second.mapping.enabled) {
        error = "mapping_disabled";
      } else if (!capability) {
        error = "port_forward_disabled";
        entry_it->second.mapping.last_error = error;
      } else if (!link || !link->is_open()) {
        error = "no_session";
        entry_it->second.mapping.last_error = error;
      } else if (streams.size() >= kMaxForwardStreams) {
        error = "stream_limit";
      } else {
        active_link = link;
        uint16_t candidate = next_stream_id;
        bool found = false;
        for (uint32_t tries = 0; tries < 65535; ++tries) {
          if (streams.find(candidate) == streams.end()) {
            found = true;
            break;
          }
          candidate = static_cast<uint16_t>(candidate == 65535 ? 1
                                                               : candidate + 1);
        }
        if (!found) {
          error = "stream_limit";
        } else {
          next_stream_id = static_cast<uint16_t>(
              candidate == 65535 ? 1 : candidate + 1);
          target_id = entry_it->second.mapping.id;
          target_host = entry_it->second.mapping.target_host;
          target_port = entry_it->second.mapping.target_port;
          created = std::make_shared<stream>();
          created->id = candidate;
          created->mapping_id = mapping_id;
          created->link = active_link;
          created->socket.fd.store(client);
          streams[candidate] = created;
          ++entry_it->second.mapping.active_streams;
          entry_it->second.mapping.last_error.clear();
        }
      }
    }
    if (!created) {
      log_error(mapping_id, error);
      close_socket(client);
      return;
    }
    log_open(target_id, created->id, target_host, target_port);
    std::thread([this, created] { stream_worker(created); }).detach();
    if (!active_link->send_text(
            encode_forward_open(created->id, target_host, target_port))) {
      close_stream(created, "send_failed");
    }
  }

  void stream_worker(std::shared_ptr<stream> state) {
    std::string reason;
    {
      std::unique_lock<std::mutex> lock(state->state_mutex);
      state->state_cv.wait_for(
          lock, std::chrono::milliseconds(kOpenTimeoutMs), [&state] {
            return state->open_ok || state->open_failed || state->closed;
          });
      if (!state->open_ok) {
        if (state->open_failed) {
          reason = state->error.empty() ? "open_failed" : state->error;
        } else if (state->closed) {
          reason = state->close_reason.empty() ? "closed" : state->close_reason;
        } else {
          reason = "open_timeout";
        }
      }
    }
    if (!reason.empty()) {
      close_stream(state, reason);
      // Tell the peer to release the stream. When the link itself is gone
      // (link_lost) there is nobody left to tell.
      if (state->link->is_open()) {
        state->link->send_text(encode_forward_close(state->id));
      }
      state->socket.close_owned();
      finish_stream(state, reason);
      return;
    }

    const socket_t fd = state->socket.fd.load();
    std::vector<uint8_t> buffer(kRelayChunkBytes);
    for (;;) {
      const int received =
          ::recv(fd, reinterpret_cast<char*>(buffer.data()),
                 static_cast<int>(buffer.size()), 0);
      if (received > 0) {
        state->bytes_in += static_cast<std::uint64_t>(received);
        add_mapping_bytes(state->mapping_id,
                          static_cast<std::uint64_t>(received), 0);
        if (!state->link->send_data(state->id, buffer.data(),
                                    static_cast<std::size_t>(received))) {
          close_stream(state, "send_failed");
          break;
        }
        continue;
      }
      if (received == 0) {
        close_stream(state, "peer_closed");
        break;
      }
      const int error = socket_error();
      if (error_interrupted(error)) continue;
      if (!state->closed.load()) close_stream(state, "recv_error");
      break;
    }

    {
      std::lock_guard<std::mutex> lock(state->state_mutex);
      if (reason.empty()) {
        reason = state->close_reason.empty() ? "closed" : state->close_reason;
      }
    }
    if (state->link->is_open()) {
      state->link->send_text(encode_forward_close(state->id));
    }
    state->socket.close_owned();
    finish_stream(state, reason);
  }

  void finish_stream(const std::shared_ptr<stream>& state,
                     const std::string& reason) {
    uint64_t id = 0;
    uint64_t mapping_id = 0;
    uint64_t total_in = 0;
    uint64_t total_out = 0;
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto it = streams.find(state->id);
      if (it == streams.end()) return;
      streams.erase(it);
      mapping_id = state->mapping_id;
      const auto entry_it = entries.find(mapping_id);
      if (entry_it != entries.end()) {
        port_mapping& mapping = entry_it->second.mapping;
        if (mapping.active_streams > 0) --mapping.active_streams;
        if (reason == "peer_closed" || reason == "peer_close") {
          mapping.last_error.clear();
        } else if (reason == "link_lost") {
          mapping.last_error = "no_session";
        } else {
          mapping.last_error = reason;
        }
        id = mapping.id;
        total_in = mapping.bytes_in;
        total_out = mapping.bytes_out;
      }
    }
    if (id == 0) id = mapping_id;
    log_close(id, total_in, total_out, reason);
    streams_cv.notify_all();
  }

  void close_stream(const std::shared_ptr<stream>& state,
                    const std::string& reason) {
    {
      std::lock_guard<std::mutex> lock(state->state_mutex);
      if (!state->closed) {
        state->closed = true;
        state->close_reason = reason;
      }
    }
    state->state_cv.notify_all();
    state->socket.request_shutdown();
  }

  bool write_client(const std::shared_ptr<stream>& state, const uint8_t* data,
                    std::size_t len) {
    std::lock_guard<std::mutex> lock(state->write_mutex);
    {
      std::lock_guard<std::mutex> state_lock(state->state_mutex);
      if (state->closed) return false;
    }
    const socket_t fd = state->socket.fd.load();
    if (fd == kInvalidSocket) return false;
    if (!send_all(fd, data, len)) return false;
    state->bytes_out += static_cast<std::uint64_t>(len);
    add_mapping_bytes(state->mapping_id, 0, static_cast<std::uint64_t>(len));
    return true;
  }

  /// Live counters for the mapping health view. The close log reads the same
  /// totals after the last chunk has been added.
  void add_mapping_bytes(uint64_t mapping_id, uint64_t bytes_in,
                         uint64_t bytes_out) {
    if (bytes_in == 0 && bytes_out == 0) return;
    std::lock_guard<std::mutex> lock(mutex);
    const auto it = entries.find(mapping_id);
    if (it == entries.end()) return;
    it->second.mapping.bytes_in += bytes_in;
    it->second.mapping.bytes_out += bytes_out;
  }

  void close_streams_for_mapping(uint64_t mapping_id,
                                 const std::string& reason) {
    std::vector<std::shared_ptr<stream>> closing;
    {
      std::lock_guard<std::mutex> lock(mutex);
      for (auto& item : streams) {
        if (item.second->mapping_id == mapping_id) {
          closing.push_back(item.second);
        }
      }
    }
    for (auto& stream : closing) close_stream(stream, reason);
  }

  bool start_mapping(uint64_t id, std::string& error) {
    port_mapping mapping;
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto it = entries.find(id);
      if (it == entries.end()) {
        error = "mapping_not_found";
        return false;
      }
      mapping = it->second.mapping;
    }
    auto listener_state =
        std::make_shared<listener>(this, id, mapping.listen_host,
                                   mapping.listen_port);
    if (!listener_state->start(error)) {
      std::lock_guard<std::mutex> lock(mutex);
      const auto it = entries.find(id);
      if (it != entries.end()) {
        it->second.mapping.listening = false;
        it->second.mapping.last_error = error;
      }
      return false;
    }
    bool removed = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto it = entries.find(id);
      if (it == entries.end()) {
        removed = true;
      } else {
        it->second.listener_state = listener_state;
        it->second.mapping.listening = true;
        it->second.mapping.listen_port = listener_state->bound_port;
        it->second.mapping.last_error.clear();
      }
    }
    if (removed) {
      // Removed while binding; do not leave a stray listener behind. Stop it
      // without the manager lock: the accept thread takes that lock too.
      listener_state->stop();
      error = "mapping_removed";
      return false;
    }
    return true;
  }

  void stop_mapping(uint64_t id) {
    std::shared_ptr<listener> listener_state;
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto it = entries.find(id);
      if (it == entries.end()) return;
      listener_state = std::move(it->second.listener_state);
      it->second.mapping.listening = false;
    }
    if (listener_state) listener_state->stop();
  }

  bool persist(std::string& error) {
    if (!store) return true;
    std::vector<port_mapping> snapshot;
    {
      std::lock_guard<std::mutex> lock(mutex);
      snapshot.reserve(entries.size());
      for (const auto& item : entries) snapshot.push_back(item.second.mapping);
    }
    std::sort(snapshot.begin(), snapshot.end(),
              [](const port_mapping& a, const port_mapping& b) {
                return a.id < b.id;
              });
    return store->save(snapshot, error);
  }

  bool normalize_and_validate(port_mapping& mapping, std::string& error) const {
    if (mapping.listen_host.empty()) mapping.listen_host = "127.0.0.1";
    if (mapping.target_host.empty()) mapping.target_host = "127.0.0.1";
    if (mapping.target_port == 0) {
      error = "target port is required";
      return false;
    }
    if (mapping.listen_port != 0 && mapping.listen_port < 1) {
      error = "invalid listen port";
      return false;
    }
    return true;
  }

  std::mutex api_mutex;
  mutable std::mutex mutex;
  std::condition_variable streams_cv;
  std::unique_ptr<port_forward_store> store;
  enabled_fn enabled;
  std::map<uint64_t, entry> entries;
  std::map<uint16_t, std::shared_ptr<stream>> streams;
  std::shared_ptr<port_forward_link> link;
  uint64_t next_id = 1;
  uint16_t next_stream_id = 1;
  bool stopping = false;
};

port_forward_manager::port_forward_manager(std::unique_ptr<port_forward_store> store,
                                           enabled_fn enabled)
    : impl_(std::make_unique<impl>(std::move(store), std::move(enabled))) {
  std::vector<port_mapping> loaded;
  std::string error;
  if (impl_->store) impl_->store->load(loaded, error);
  if (!error.empty()) log_error(0, error);
  uint64_t max_id = 0;
  for (port_mapping& mapping : loaded) {
    if (mapping.id > max_id) max_id = mapping.id;
    impl_->entries[mapping.id] = impl::entry{mapping, nullptr};
  }
  impl_->next_id = max_id + 1;
  // Persisted rows only start listening when the capability is on. A mapping
  // that was enabled last run stays enabled in the UI; it just shows
  // port_forward_disabled until the user turns the switch on.
  if (impl_->allowed()) {
    for (const auto& item : impl_->entries) {
      if (item.second.mapping.enabled) {
        std::string start_error;
        impl_->start_mapping(item.first, start_error);
      }
    }
  } else {
    for (auto& item : impl_->entries) {
      if (item.second.mapping.enabled) {
        item.second.mapping.last_error = "port_forward_disabled";
      }
    }
  }
}

port_forward_manager::~port_forward_manager() = default;

bool port_forward_manager::add(port_mapping& mapping, std::string& error) {
  std::lock_guard<std::mutex> api(impl_->api_mutex);
  error.clear();
  if (!impl_->normalize_and_validate(mapping, error)) return false;
  if (mapping.enabled && !impl_->allowed()) {
    error = "port_forward_disabled";
    return false;
  }
  uint64_t id = 0;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    id = impl_->next_id++;
    mapping.id = id;
    mapping.listening = false;
    mapping.active_streams = 0;
    mapping.last_error.clear();
    impl_->entries[id] = impl::entry{mapping, nullptr};
  }
  if (mapping.enabled) {
    std::string start_error;
    if (!impl_->start_mapping(id, start_error)) {
      // A mapping that could not bind is not persisted: the caller gets the
      // reason and can fix the port, and a restart does not keep retrying a
      // dead endpoint.
      {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->entries.erase(id);
      }
      error = start_error;
      return false;
    }
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->entries.find(id);
    if (it != impl_->entries.end()) {
      mapping = it->second.mapping;
      mapping.last_error.clear();
    }
  }
  std::string save_error;
  if (!impl_->persist(save_error)) {
    // Roll the listener and the in-memory row back as well: an add that
    // returns false must not leave a silently active mapping behind.
    impl_->stop_mapping(id);
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      impl_->entries.erase(id);
    }
    error = save_error;
    return false;
  }
  return true;
}

bool port_forward_manager::update(const port_mapping& mapping,
                                  std::string& error) {
  std::lock_guard<std::mutex> api(impl_->api_mutex);
  error.clear();
  port_mapping next = mapping;
  if (!impl_->normalize_and_validate(next, error)) return false;
  if (next.enabled && !impl_->allowed()) {
    error = "port_forward_disabled";
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->entries.find(next.id);
    if (it == impl_->entries.end()) {
      error = "mapping_not_found";
      return false;
    }
    // Preserve live counters across a configuration edit.
    next.bytes_in = it->second.mapping.bytes_in;
    next.bytes_out = it->second.mapping.bytes_out;
    next.active_streams = it->second.mapping.active_streams;
  }
  impl_->stop_mapping(next.id);
  impl_->close_streams_for_mapping(next.id, "mapping_changed");
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->entries.find(next.id);
    if (it == impl_->entries.end()) {
      error = "mapping_not_found";
      return false;
    }
    next.listening = false;
    next.last_error.clear();
    it->second.mapping = next;
  }
  if (next.enabled) {
    std::string start_error;
    if (!impl_->start_mapping(next.id, start_error)) error = start_error;
  }
  std::string save_error;
  if (!impl_->persist(save_error) && error.empty()) error = save_error;
  return error.empty();
}

bool port_forward_manager::remove(uint64_t id) {
  std::string error;
  return remove(id, error);
}

bool port_forward_manager::remove(uint64_t id, std::string& error) {
  std::lock_guard<std::mutex> api(impl_->api_mutex);
  error.clear();
  std::shared_ptr<impl::listener> listener_state;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->entries.find(id);
    if (it == impl_->entries.end()) {
      error = "mapping_not_found";
      return false;
    }
    listener_state = std::move(it->second.listener_state);
    impl_->entries.erase(it);
  }
  if (listener_state) listener_state->stop();
  impl_->close_streams_for_mapping(id, "mapping_removed");
  std::string save_error;
  if (!impl_->persist(save_error)) {
    error = save_error;
    return false;
  }
  return true;
}

bool port_forward_manager::set_enabled(uint64_t id, bool enabled,
                                       std::string& error) {
  error.clear();
  port_mapping mapping;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->entries.find(id);
    if (it == impl_->entries.end()) {
      error = "mapping_not_found";
      return false;
    }
    mapping = it->second.mapping;
  }
  mapping.enabled = enabled;
  return update(mapping, error);
}

bool port_forward_manager::set_target(uint64_t id,
                                      const std::string& target_host,
                                      uint16_t target_port,
                                      std::string& error) {
  std::lock_guard<std::mutex> api(impl_->api_mutex);
  error.clear();
  if (target_host.empty() || target_host.size() > 255) {
    error = "invalid target host";
    return false;
  }
  if (target_port == 0) {
    error = "invalid target port";
    return false;
  }
  std::string previous_host;
  uint16_t previous_port = 0;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->entries.find(id);
    if (it == impl_->entries.end()) {
      error = "mapping_not_found";
      return false;
    }
    port_mapping& mapping = it->second.mapping;
    previous_host = mapping.target_host;
    previous_port = mapping.target_port;
    mapping.target_host = target_host;
    mapping.target_port = target_port;
    mapping.last_error.clear();
  }
  std::string save_error;
  if (!impl_->persist(save_error)) {
    // The in-memory rule must not outlive the failed write: a restart would
    // restore the old target anyway, so show the same thing now.
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->entries.find(id);
    if (it != impl_->entries.end()) {
      it->second.mapping.target_host = previous_host;
      it->second.mapping.target_port = previous_port;
    }
    error = save_error;
    return false;
  }
  return true;
}

std::size_t port_forward_manager::refresh() {
  std::lock_guard<std::mutex> api(impl_->api_mutex);
  const bool allowed = impl_->allowed();
  std::vector<std::pair<uint64_t, bool>> work;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& item : impl_->entries) {
      if (!item.second.mapping.enabled) continue;
      const bool running = item.second.listener_state != nullptr;
      if (allowed && !running) work.emplace_back(item.first, true);
      if (!allowed && running) work.emplace_back(item.first, false);
    }
  }
  for (const auto& item : work) {
    if (item.second) {
      std::string error;
      impl_->start_mapping(item.first, error);
    } else {
      impl_->stop_mapping(item.first);
      std::lock_guard<std::mutex> lock(impl_->mutex);
      const auto it = impl_->entries.find(item.first);
      if (it != impl_->entries.end()) {
        it->second.mapping.last_error = "port_forward_disabled";
      }
    }
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::size_t running = 0;
    for (const auto& item : impl_->entries) {
      if (item.second.listener_state && item.second.mapping.listening) ++running;
    }
    return running;
  }
}

std::vector<port_mapping> port_forward_manager::mappings() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<port_mapping> out;
  out.reserve(impl_->entries.size());
  for (const auto& item : impl_->entries) {
    port_mapping mapping = item.second.mapping;
    mapping.listening = item.second.listener_state != nullptr && mapping.listening;
    out.push_back(std::move(mapping));
  }
  std::sort(out.begin(), out.end(),
            [](const port_mapping& a, const port_mapping& b) {
              return a.id < b.id;
            });
  return out;
}

bool port_forward_manager::get(uint64_t id, port_mapping& out) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto it = impl_->entries.find(id);
  if (it == impl_->entries.end()) return false;
  out = it->second.mapping;
  out.listening = it->second.listener_state != nullptr && out.listening;
  return true;
}

void port_forward_manager::attach_link(
    std::shared_ptr<port_forward_link> link) {
  std::vector<std::shared_ptr<impl::stream>> live;
  std::shared_ptr<port_forward_link> replaced;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->link != link) {
      replaced = std::move(impl_->link);
      // Streams belong to the old link. Reconnect attaches a fresh one; they
      // can never be resumed, so close them instead of waiting for a timeout.
      for (auto& item : impl_->streams) live.push_back(item.second);
    }
    impl_->link = std::move(link);
  }
  if (replaced) {
    replaced->mark_closed();
    for (auto& stream : live) impl_->close_stream(stream, "link_replaced");
  }
}

void port_forward_manager::detach_link() {
  detach_link_if(nullptr);
}

void port_forward_manager::detach_link_if(const port_forward_link* link) {
  std::vector<std::shared_ptr<impl::stream>> live;
  std::shared_ptr<port_forward_link> old_link;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->link) return;
    if (link != nullptr && impl_->link.get() != link) return;
    old_link = std::move(impl_->link);
    for (auto& item : impl_->streams) live.push_back(item.second);
  }
  // Latch closed before the workers inspect it: is_open() must not touch the
  // loop-affine session transport from a relay thread.
  if (old_link) old_link->mark_closed();
  for (auto& stream : live) impl_->close_stream(stream, "link_lost");
}

bool port_forward_manager::linked() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->link && impl_->link->is_open();
}

void port_forward_manager::on_control(const std::string& text) {
  port_forward_message message;
  std::string error;
  if (!decode_port_forward_message(text, message, error)) {
    log_error(0, error);
    return;
  }
  std::shared_ptr<impl::stream> state;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->streams.find(message.stream);
    if (it != impl_->streams.end()) state = it->second;
  }
  if (!state) return;
  if (message.type == "forward-open-ok") {
    {
      std::lock_guard<std::mutex> lock(state->state_mutex);
      state->open_ok = true;
    }
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      const auto entry_it = impl_->entries.find(state->mapping_id);
      if (entry_it != impl_->entries.end()) {
        entry_it->second.mapping.last_error.clear();
      }
    }
    state->state_cv.notify_all();
  } else if (message.type == "forward-open-fail") {
    {
      std::lock_guard<std::mutex> lock(state->state_mutex);
      state->open_failed = true;
      state->error = message.error.empty() ? "open_failed" : message.error;
    }
    state->state_cv.notify_all();
  } else if (message.type == "forward-close") {
    impl_->close_stream(state, "peer_close");
  }
}

void port_forward_manager::on_data(uint16_t stream, const uint8_t* data,
                                   std::size_t len) {
  std::shared_ptr<impl::stream> state;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->streams.find(stream);
    if (it != impl_->streams.end()) state = it->second;
  }
  if (!state) return;
  if (!impl_->write_client(state, data, len)) {
    impl_->close_stream(state, "client_write_failed");
  }
}

// ---------------------------------------------------------------------------
// Target-side handler
// ---------------------------------------------------------------------------

struct port_forward_host::impl {
  using enabled_fn = port_forward_host::enabled_fn;

  struct stream {
    uint16_t id = 0;
    socket_state socket;
    std::mutex write_mutex;
    std::atomic<bool> connected{false};
    std::atomic<bool> closed{false};
    std::mutex state_mutex;
    std::string close_reason;
  };

  impl(std::shared_ptr<port_forward_link> link_value, enabled_fn enabled_value)
      : link(std::move(link_value)), enabled(std::move(enabled_value)) {}

  ~impl() { stop(); }

  bool allowed() const { return enabled && enabled(); }

  void send_text(const std::string& text) {
    if (link) link->send_text(text);
  }

  void start_open(uint16_t id, const std::string& host, uint16_t port) {
    if (!allowed()) {
      send_text(encode_forward_open_fail(id, "port_forward_disabled"));
      return;
    }
    if (host.empty() || port == 0) {
      send_text(encode_forward_open_fail(id, "bad_target"));
      return;
    }
    std::shared_ptr<stream> state;
    const char* refusal = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (stopping) {
        refusal = "shutting_down";
      } else if (streams.find(id) != streams.end()) {
        refusal = "stream_in_use";
      } else if (streams.size() >= kMaxForwardStreams) {
        refusal = "stream_limit";
      } else {
        state = std::make_shared<stream>();
        state->id = id;
        streams[id] = state;
      }
    }
    if (refusal != nullptr) {
      send_text(encode_forward_open_fail(id, refusal));
      return;
    }
    std::thread([this, state, host, port] { worker(state, host, port); })
        .detach();
  }

  void worker(std::shared_ptr<stream> state, const std::string& host,
              uint16_t port) {
    std::string error;
    const socket_t fd = connect_target(host, port, state->closed, error);
    if (fd == kInvalidSocket) {
      send_text(encode_forward_open_fail(
          state->id, error.empty() ? "connect_failed" : error));
      state->socket.close_owned();
      finish(state->id);
      return;
    }
    {
      std::lock_guard<std::mutex> lock(state->socket.mutex);
      if (state->closed.load()) {
        close_socket(fd);
        state->socket.closed = true;
        finish(state->id);
        return;
      }
      state->socket.fd.store(fd);
    }
    if (!link || !link->is_open() ||
        !link->send_text(encode_forward_open_ok(state->id))) {
      close_state(state, "send_failed");
      state->socket.close_owned();
      finish(state->id);
      return;
    }
    state->connected.store(true);

    std::vector<uint8_t> buffer(kRelayChunkBytes);
    std::string reason;
    for (;;) {
      const int received =
          ::recv(fd, reinterpret_cast<char*>(buffer.data()),
                 static_cast<int>(buffer.size()), 0);
      if (received > 0) {
        if (!link->is_open() ||
            !link->send_data(state->id, buffer.data(),
                             static_cast<std::size_t>(received))) {
          reason = "send_failed";
          break;
        }
        continue;
      }
      if (received == 0) {
        reason = "peer_closed";
        break;
      }
      const int error_code = socket_error();
      if (error_interrupted(error_code)) continue;
      reason = state->closed.load() ? "closed" : "recv_error";
      break;
    }
    if (link && link->is_open()) {
      link->send_text(encode_forward_close(state->id));
    }
    close_state(state, reason.empty() ? "closed" : reason);
    state->socket.close_owned();
    finish(state->id);
  }

  void close_state(const std::shared_ptr<stream>& state,
                   const std::string& reason) {
    state->closed.store(true);
    {
      std::lock_guard<std::mutex> lock(state->state_mutex);
      if (state->close_reason.empty()) state->close_reason = reason;
    }
    state->socket.request_shutdown();
  }

  bool write_target(const std::shared_ptr<stream>& state, const uint8_t* data,
                    std::size_t len) {
    std::lock_guard<std::mutex> lock(state->write_mutex);
    if (state->closed.load()) return false;
    const socket_t fd = state->socket.fd.load();
    if (fd == kInvalidSocket) return false;
    return send_all(fd, data, len);
  }

  void finish(uint16_t id) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      streams.erase(id);
    }
    streams_cv.notify_all();
  }

  void stop() {
    if (link) link->mark_closed();
    std::vector<std::shared_ptr<stream>> live;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (stopping && streams.empty()) return;
      stopping = true;
      for (auto& item : streams) live.push_back(item.second);
    }
    for (auto& state : live) close_state(state, "session_closed");
    std::unique_lock<std::mutex> lock(mutex);
    streams_cv.wait(lock, [this] { return streams.empty(); });
  }

  std::shared_ptr<port_forward_link> link;
  enabled_fn enabled;
  mutable std::mutex mutex;
  std::condition_variable streams_cv;
  std::map<uint16_t, std::shared_ptr<stream>> streams;
  bool stopping = false;
};

port_forward_host::port_forward_host(
    std::shared_ptr<port_forward_link> link, enabled_fn enabled)
    : impl_(std::make_unique<impl>(std::move(link), std::move(enabled))) {}

port_forward_host::~port_forward_host() = default;

void port_forward_host::on_control(const std::string& text) {
  port_forward_message message;
  std::string error;
  if (!decode_port_forward_message(text, message, error)) return;
  if (message.type == "forward-open") {
    impl_->start_open(message.stream, message.host, message.port);
  } else if (message.type == "forward-close") {
    std::shared_ptr<impl::stream> state;
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      const auto it = impl_->streams.find(message.stream);
      if (it != impl_->streams.end()) state = it->second;
    }
    if (state) impl_->close_state(state, "peer_close");
  }
}

void port_forward_host::on_data(uint16_t stream, const uint8_t* data,
                                std::size_t len) {
  std::shared_ptr<impl::stream> state;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->streams.find(stream);
    if (it != impl_->streams.end()) state = it->second;
  }
  if (!state) return;
  if (!impl_->write_target(state, data, len)) {
    impl_->close_state(state, "target_write_failed");
  }
}

std::size_t port_forward_host::active_streams() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->streams.size();
}

void port_forward_host::stop() { impl_->stop(); }

}  // namespace molink
