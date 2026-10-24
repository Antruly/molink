#include "util.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cctype>
#include <chrono>

namespace molink {
namespace {

constexpr char kHex[] = "0123456789abcdef";

}  // namespace

uint64_t epoch_ms() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch())
          .count());
}

uint64_t monotonic_ms() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
          .count());
}

std::string random_bytes(std::size_t count) {
  std::string out(count, '\0');
  if (count == 0) return out;
  if (RAND_bytes(reinterpret_cast<unsigned char*>(out.data()),
                 static_cast<int>(count)) != 1) {
    return std::string();
  }
  return out;
}

std::string random_hex(std::size_t count) {
  const std::string bytes = random_bytes(count);
  if (bytes.empty()) return std::string();
  std::string out;
  out.reserve(bytes.size() * 2);
  for (unsigned char byte : bytes) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 0x0f]);
  }
  return out;
}

std::string sha256_hex(const std::string& data) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  if (EVP_Digest(data.data(), data.size(), digest, &length, EVP_sha256(),
                 nullptr) != 1) {
    return std::string();
  }
  std::string out;
  out.reserve(length * 2);
  for (unsigned int i = 0; i < length; ++i) {
    out.push_back(kHex[digest[i] >> 4]);
    out.push_back(kHex[digest[i] & 0x0f]);
  }
  return out;
}

bool constant_time_equals(const std::string& left, const std::string& right) {
  if (left.size() != right.size()) return false;
  if (left.empty()) return true;
  return CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

std::string trim(std::string value) {
  while (!value.empty() &&
         (value.back() == ' ' || value.back() == '\t' ||
          value.back() == '\r' || value.back() == '\n')) {
    value.pop_back();
  }
  std::size_t start = 0;
  while (start < value.size() &&
         (value[start] == ' ' || value[start] == '\t' || value[start] == '\r' ||
          value[start] == '\n')) {
    ++start;
  }
  return value.substr(start);
}

std::string lowercase_ascii(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

std::vector<std::string> split(const std::string& value, char separator) {
  std::vector<std::string> out;
  std::size_t begin = 0;
  while (begin <= value.size()) {
    const std::size_t end = value.find(separator, begin);
    if (end == std::string::npos) {
      out.push_back(value.substr(begin));
      break;
    }
    out.push_back(value.substr(begin, end - begin));
    begin = end + 1;
  }
  return out;
}

bool has_whitespace_or_control(const std::string& value) {
  for (unsigned char c : value) {
    if (c <= 0x20 || c == 0x7f) return true;
  }
  return false;
}

std::string url_encode(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0f]);
    }
  }
  return out;
}

int clamp_int(long long value, int minimum, int maximum, int fallback) {
  if (value < minimum || value > maximum) return fallback;
  return static_cast<int>(value);
}

}  // namespace molink
