#include "util.h"

#include <chrono>
#include <random>

namespace molink {

bool is_loopback_host(const std::string& host) {
  return host == "127.0.0.1" || host == "::1" || host == "localhost";
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

uint64_t now_ms() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
          .count());
}

}  // namespace molink
