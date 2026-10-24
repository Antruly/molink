#pragma once

#include <string>

namespace molink {

struct password_hash {
  std::string algorithm = "pbkdf2-sha256";
  int iterations = 600000;
  std::string salt_hex;
  std::string hash_hex;
};

/// PBKDF2-HMAC-SHA256 with a random 16-byte salt and 32-byte output, run on
/// the caller's thread (always a libuv worker, never an event loop).
bool hash_password(const std::string& password, password_hash& out,
                   std::string& error);
bool verify_password(const std::string& password, const password_hash& stored);

std::string encode_password_hash(const password_hash& hash);
bool decode_password_hash(const std::string& text, password_hash& out);

/// 10..256 bytes, no control characters, not all whitespace.
bool password_acceptable(const std::string& password, std::string& error);

/// A hash used to equalise timing when the username does not exist.
const password_hash& dummy_password_hash();

}  // namespace molink
