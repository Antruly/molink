// RFC 6238 (TOTP) vectors, the ±1 step window, base32 and the sealed-at-rest
// secret. Pure code: no config directory, no network, no libuvcpp.
#include <cstdint>
#include <cstdio>
#include <string>

#include "totp.h"

namespace {

int checks = 0;
int failures = 0;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

using molink::totp::base32_decode;
using molink::totp::base32_encode;
using molink::totp::code_for;
using molink::totp::open;
using molink::totp::seal;
using molink::totp::verify;

void test_rfc6238_vectors() {
  // The RFC's SHA-1 seed is the ASCII string below, 20 bytes.
  const std::string secret = "12345678901234567890";
  struct vector {
    uint64_t time;
    const char* code;  // last six digits of the RFC's eight-digit value
  };
  const vector vectors[] = {
      {59u, "287082"},
      {1111111109u, "081804"},
      {1111111111u, "050471"},
      {1234567890u, "005924"},
      {2000000000u, "279037"},
      {20000000000u, "353130"},
  };
  for (const vector& item : vectors) {
    const std::string actual = code_for(secret, item.time);
    if (actual != item.code) {
      std::fprintf(stderr, "  t=%llu expected %s got %s\n",
                   static_cast<unsigned long long>(item.time), item.code,
                   actual.c_str());
    }
    expect(actual == item.code, "RFC 6238 SHA-1 vector");
  }
}

void test_window_and_rejection() {
  const std::string secret = "12345678901234567890";
  const uint64_t now = 1111111111u;
  expect(verify(secret, code_for(secret, now), now), "current step accepted");
  expect(verify(secret, code_for(secret, now - 30), now),
         "previous step accepted");
  expect(verify(secret, code_for(secret, now + 30), now),
         "next step accepted");
  expect(!verify(secret, code_for(secret, now - 60), now),
         "two steps back rejected");
  expect(!verify(secret, code_for(secret, now + 60), now),
         "two steps ahead rejected");
  expect(!verify(secret, "000000", now), "wrong code rejected");
  expect(!verify(secret, "12345", now), "short code rejected");
  expect(!verify(secret, "1234567", now), "long code rejected");
  expect(!verify(secret, "12a456", now), "non-digit rejected");
  expect(verify(secret, "  " + code_for(secret, now) + " ", now),
         "surrounding spaces tolerated");
  expect(!verify(std::string(), code_for(secret, now), now),
         "empty secret rejected");
}

void test_base32() {
  const std::string secret = "12345678901234567890";
  const std::string encoded = base32_encode(secret);
  expect(encoded == "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ",
         "base32 matches the RFC 4648 alphabet");
  std::string decoded;
  expect(base32_decode(encoded, decoded), "base32 decodes");
  expect(decoded == secret, "base32 round-trip");
  // Authenticator apps often include padding and lowercase groups.
  std::string padded;
  expect(base32_decode("gezd gnbv gy3t qojq gezd gnbv gy3t qojq=====", padded),
         "lowercase/padded base32 decodes");
  expect(padded == secret, "normalized base32 round-trip");
  std::string junk;
  expect(!base32_decode("GEZD!NBV", junk), "invalid base32 rejected");
}

void test_secret_and_uri() {
  const std::string first = molink::totp::generate_secret();
  const std::string second = molink::totp::generate_secret();
  expect(first.size() == molink::totp::kSecretBytes,
         "secret has the RFC length");
  expect(first != second, "two generated secrets differ");
  const std::string uri = molink::totp::otpauth_uri(
      base32_encode(first), "my host", "moLink");
  expect(uri.rfind("otpauth://totp/moLink:my%20host?", 0) == 0,
         "otpauth URI has the issuer and an encoded account");
  expect(uri.find("algorithm=SHA1") != std::string::npos,
         "otpauth URI names SHA1");
  expect(uri.find("digits=6") != std::string::npos,
         "otpauth URI names six digits");
  expect(uri.find("period=30") != std::string::npos,
         "otpauth URI names the 30 s period");
  expect(uri.find("secret=" + base32_encode(first)) != std::string::npos,
         "otpauth URI carries the base32 secret");
}

void test_sealed_storage() {
  const std::string key = molink::totp::random_bytes(32);
  const std::string secret = molink::totp::generate_secret();
  const std::string sealed = seal(secret, key);
  expect(sealed != secret, "sealed value is not the plaintext");
  expect(sealed.find(secret) == std::string::npos,
         "sealed value does not embed the plaintext");
  std::string opened;
  expect(open(sealed, key, opened), "sealed secret opens with its key");
  expect(opened == secret, "sealed secret round-trips");
  expect(!open(sealed, molink::totp::random_bytes(32), opened),
         "wrong key rejected");
  expect(!open("not-hex", key, opened), "malformed blob rejected");
  std::string tampered = sealed;
  // Flip one hex digit in the ciphertext, not the nonce.
  const std::size_t at = tampered.size() / 2;
  tampered[at] = tampered[at] == '0' ? '1' : '0';
  expect(!open(tampered, key, opened), "tampered ciphertext rejected");
}

}  // namespace

int main() {
  test_rfc6238_vectors();
  test_window_and_rejection();
  test_base32();
  test_secret_and_uri();
  test_sealed_storage();
  if (failures == 0) {
    std::printf("totp: %d checks passed\n", checks);
    return 0;
  }
  std::fprintf(stderr, "totp: %d/%d checks failed\n", failures, checks);
  return 1;
}
