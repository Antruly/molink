#include "util.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <random>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace molink {

bool is_loopback_host(const std::string& host) {
  return host == "127.0.0.1" || host == "::1" || host == "localhost";
}

std::string format_url_host(const std::string& host) {
  if (host.empty()) return "127.0.0.1";
  if (host.front() == '[' && host.back() == ']') return host;
  return host.find(':') != std::string::npos ? "[" + host + "]" : host;
}

bool is_loopback_peer(const std::string& peer) {
  return peer == "127.0.0.1" || peer == "::1" ||
         peer == "::ffff:127.0.0.1" || peer == "localhost";
}

std::string make_session_public_id() {
  static const char kAlphabet[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
  static constexpr std::size_t kAlphabetSize = sizeof(kAlphabet) - 1;

  std::random_device rd;
  std::string id;
  id.reserve(11);
  for (int group = 0; group < 3; ++group) {
    if (group != 0) id.push_back('-');
    for (int i = 0; i < 3; ++i) {
      id.push_back(kAlphabet[rd() % kAlphabetSize]);
    }
  }
  return id;
}

std::vector<uint32_t> utf8_codepoints(const std::string& text) {
  std::vector<uint32_t> out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    uint32_t cp = 0;
    int extra = 0;
    if (lead < 0x80) {
      cp = lead;
      extra = 0;
    } else if ((lead & 0xe0) == 0xc0) {
      cp = lead & 0x1f;
      extra = 1;
    } else if ((lead & 0xf0) == 0xe0) {
      cp = lead & 0x0f;
      extra = 2;
    } else if ((lead & 0xf8) == 0xf0) {
      cp = lead & 0x07;
      extra = 3;
    } else {
      ++i;
      continue;
    }
    if (i + static_cast<std::size_t>(extra) >= text.size()) break;
    bool valid = true;
    for (int j = 1; j <= extra; ++j) {
      const unsigned char c = static_cast<unsigned char>(text[i + j]);
      if ((c & 0xc0) != 0x80) {
        valid = false;
        break;
      }
      cp = (cp << 6) | (c & 0x3f);
    }
    if (valid && cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff)) {
      out.push_back(cp);
    }
    i += static_cast<std::size_t>(extra) + 1;
  }
  return out;
}

uint64_t now_ms() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
          .count());
}

std::string base64_encode(const uint8_t* data, std::size_t len) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  std::size_t i = 0;
  for (; i + 2 < len; i += 3) {
    const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                       (static_cast<uint32_t>(data[i + 1]) << 8) |
                       static_cast<uint32_t>(data[i + 2]);
    out.push_back(kAlphabet[(v >> 18) & 0x3f]);
    out.push_back(kAlphabet[(v >> 12) & 0x3f]);
    out.push_back(kAlphabet[(v >> 6) & 0x3f]);
    out.push_back(kAlphabet[v & 0x3f]);
  }
  if (i < len) {
    uint32_t v = static_cast<uint32_t>(data[i]) << 16;
    const bool two = i + 1 < len;
    if (two) v |= static_cast<uint32_t>(data[i + 1]) << 8;
    out.push_back(kAlphabet[(v >> 18) & 0x3f]);
    out.push_back(kAlphabet[(v >> 12) & 0x3f]);
    out.push_back(two ? kAlphabet[(v >> 6) & 0x3f] : '=');
    out.push_back('=');
  }
  return out;
}

std::string url_encode(const std::string& value) {
  static const char hex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size());
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0x0f]);
    }
  }
  return out;
}

std::vector<std::string> local_ipv4_addresses() {
  std::vector<std::string> out;
  auto add = [&out](const std::string& value) {
    if (value.empty() || value.rfind("127.", 0) == 0) return;
    if (std::find(out.begin(), out.end(), value) == out.end()) {
      out.push_back(value);
    }
  };

#if defined(_WIN32)
  ULONG size = 16 * 1024;
  std::vector<unsigned char> storage(size);
  auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
  ULONG result = GetAdaptersAddresses(
      AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                   GAA_FLAG_SKIP_DNS_SERVER,
      nullptr, addresses, &size);
  if (result == ERROR_BUFFER_OVERFLOW) {
    storage.resize(size);
    addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    result = GetAdaptersAddresses(
        AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                     GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, addresses, &size);
  }
  if (result == NO_ERROR) {
    for (auto* adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
      if (adapter->OperStatus != IfOperStatusUp) continue;
      for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr;
           unicast = unicast->Next) {
        const sockaddr* address = unicast->Address.lpSockaddr;
        if (address == nullptr || address->sa_family != AF_INET) continue;
        char text[INET_ADDRSTRLEN] = {};
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
        if (inet_ntop(AF_INET, &ipv4->sin_addr, text, sizeof(text)) != nullptr) {
          add(text);
        }
      }
    }
  }
#else
  struct ifaddrs* interfaces = nullptr;
  if (getifaddrs(&interfaces) == 0) {
    for (struct ifaddrs* item = interfaces; item != nullptr;
         item = item->ifa_next) {
      if (item->ifa_addr == nullptr) continue;
      if (!(item->ifa_flags & IFF_UP) || (item->ifa_flags & IFF_LOOPBACK)) {
        continue;
      }
      if (item->ifa_name != nullptr &&
          std::strcmp(item->ifa_name, "docker0") == 0) {
        continue;
      }
      if (item->ifa_addr->sa_family != AF_INET) continue;
      char text[INET_ADDRSTRLEN] = {};
      const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
      if (inet_ntop(AF_INET, &ipv4->sin_addr, text, sizeof(text)) != nullptr) {
        add(text);
      }
    }
    freeifaddrs(interfaces);
  }
#endif

  const auto is_private = [](const std::string& value) {
    if (value.rfind("10.", 0) == 0 || value.rfind("192.168.", 0) == 0) {
      return true;
    }
    if (value.rfind("172.", 0) == 0) {
      const int second = std::atoi(value.c_str() + 4);
      return second >= 16 && second <= 31;
    }
    return false;
  };
  std::stable_sort(out.begin(), out.end(),
                   [&](const std::string& a, const std::string& b) {
                     return is_private(a) && !is_private(b);
                   });
  return out;
}
}  // namespace molink
