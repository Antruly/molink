#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace molink {
namespace totp {

/// RFC 6238 defaults: a 20-byte SHA-1 secret, six digits and a 30 s step.
constexpr std::size_t kSecretBytes = 20;
constexpr int kDigits = 6;
constexpr uint64_t kStepSeconds = 30;
/// Accept one step before and after the current one, so a small clock drift
/// between the host and the authenticator does not reject a valid code.
constexpr int kDefaultWindow = 1;

/// Cryptographically random bytes from the platform source (/dev/urandom on
/// POSIX, std::random_device elsewhere).
std::string random_bytes(std::size_t count);

/// RFC 4648 base32 without padding, the alphabet authenticator apps expect.
std::string base32_encode(const std::string& raw);
bool base32_decode(const std::string& text, std::string& raw);

/// A fresh 20-byte raw secret.
std::string generate_secret();

/// The six-digit code for one time step.
std::string code_for(const std::string& secret, uint64_t unix_seconds);

/// True when @p code matches the current step or one either side. The input is
/// normalized for spaces; anything that is not six digits is rejected.
bool verify(const std::string& secret, const std::string& code,
            uint64_t unix_seconds, int window = kDefaultWindow);

/// otpauth:// URI for an authenticator app. @p secret is the base32 form.
std::string otpauth_uri(const std::string& secret_base32,
                        const std::string& account,
                        const std::string& issuer = "moLink");

/// Authenticated at-rest encryption for the secret. HMAC-SHA1 is used as a
/// PRF in counter mode plus an encrypt-then-MAC tag, so no new dependency is
/// introduced. The output is hex(nonce || ciphertext || tag).
std::string seal(const std::string& plaintext, const std::string& key);
bool open(const std::string& sealed, const std::string& key,
          std::string& plaintext);

}  // namespace totp
}  // namespace molink
