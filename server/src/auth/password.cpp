#include "auth/password.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cctype>
#include <vector>

#include "util.h"

namespace molink {
namespace {

constexpr int kIterations = 600000;
constexpr std::size_t kSaltBytes = 16;
constexpr std::size_t kHashBytes = 32;

bool from_hex(const std::string& text, std::string& out) {
  if (text.size() % 2 != 0) return false;
  out.assign(text.size() / 2, '\0');
  auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < text.size(); i += 2) {
    const int high = digit(text[i]);
    const int low = digit(text[i + 1]);
    if (high < 0 || low < 0) return false;
    out[i / 2] = static_cast<char>((high << 4) | low);
  }
  return true;
}

std::string to_hex(const unsigned char* data, std::size_t size) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    out.push_back(kHex[data[i] >> 4]);
    out.push_back(kHex[data[i] & 0x0f]);
  }
  return out;
}

bool derive(const std::string& password, const std::string& salt,
            int iterations, std::string& out) {
  unsigned char digest[kHashBytes];
  const int rc = PKCS5_PBKDF2_HMAC(
      password.data(), static_cast<int>(password.size()),
      reinterpret_cast<const unsigned char*>(salt.data()),
      static_cast<int>(salt.size()), iterations, EVP_sha256(),
      static_cast<int>(sizeof(digest)), digest);
  if (rc != 1) return false;
  out = to_hex(digest, sizeof(digest));
  return true;
}

}  // namespace

bool hash_password(const std::string& password, password_hash& out,
                   std::string& error) {
  if (!password_acceptable(password, error)) return false;
  const std::string salt = random_bytes(kSaltBytes);
  if (salt.size() != kSaltBytes) {
    error = "secure random generator is unavailable";
    return false;
  }
  out.algorithm = "pbkdf2-sha256";
  out.iterations = kIterations;
  out.salt_hex = to_hex(reinterpret_cast<const unsigned char*>(salt.data()),
                        salt.size());
  if (!derive(password, salt, out.iterations, out.hash_hex)) {
    error = "password hashing failed";
    return false;
  }
  return true;
}

bool verify_password(const std::string& password, const password_hash& stored) {
  if (stored.algorithm != "pbkdf2-sha256") return false;
  if (stored.iterations < 1000 || stored.iterations > 10000000) return false;
  std::string salt;
  if (!from_hex(stored.salt_hex, salt)) return false;
  std::string actual;
  if (!derive(password, salt, stored.iterations, actual)) return false;
  return constant_time_equals(actual, stored.hash_hex);
}

std::string encode_password_hash(const password_hash& hash) {
  return hash.algorithm + "$" + std::to_string(hash.iterations) + "$" +
         hash.salt_hex + "$" + hash.hash_hex;
}

bool decode_password_hash(const std::string& text, password_hash& out) {
  const std::vector<std::string> parts = split(text, '$');
  if (parts.size() != 4) return false;
  if (parts[0] != "pbkdf2-sha256") return false;
  int iterations = 0;
  try {
    iterations = std::stoi(parts[1]);
  } catch (...) {
    return false;
  }
  if (iterations < 1000 || iterations > 10000000) return false;
  if (parts[2].empty() || parts[3].empty()) return false;
  out.algorithm = parts[0];
  out.iterations = iterations;
  out.salt_hex = parts[2];
  out.hash_hex = parts[3];
  return true;
}

bool password_acceptable(const std::string& password, std::string& error) {
  if (password.size() < 10) {
    error = "password must be at least 10 characters";
    return false;
  }
  if (password.size() > 256) {
    error = "password must be at most 256 characters";
    return false;
  }
  for (unsigned char c : password) {
    if (c < 0x20 || c == 0x7f) {
      error = "password must not contain control characters";
      return false;
    }
  }
  if (trim(password).empty()) {
    error = "password must not be blank";
    return false;
  }
  return true;
}

const password_hash& dummy_password_hash() {
  static const password_hash kDummy = [] {
    password_hash hash;
    hash.iterations = kIterations;
    hash.salt_hex = "000102030405060708090a0b0c0d0e0f";
    hash.hash_hex =
        "9c8b7e6d5a4f3e2d1c0b9a8f7e6d5c4b3a29180706050403020100ffeeddccbb";
    return hash;
  }();
  return kDummy;
}

}  // namespace molink
