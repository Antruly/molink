#include "totp.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <random>

namespace molink {
namespace totp {
namespace {

inline uint32_t rotate_left(uint32_t value, int bits) {
  return (value << bits) | (value >> (32 - bits));
}

struct sha1_ctx {
  uint32_t h[5] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u,
                   0xc3d2e1f0u};
  uint8_t buffer[64] = {};
  uint64_t total_bytes = 0;
  std::size_t used = 0;
};

void sha1_block(sha1_ctx& ctx, const uint8_t* block) {
  uint32_t w[80];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
           (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<uint32_t>(block[i * 4 + 3]);
  }
  for (int i = 16; i < 80; ++i) {
    w[i] = rotate_left(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  }
  uint32_t a = ctx.h[0];
  uint32_t b = ctx.h[1];
  uint32_t c = ctx.h[2];
  uint32_t d = ctx.h[3];
  uint32_t e = ctx.h[4];
  for (int i = 0; i < 80; ++i) {
    uint32_t f = 0;
    uint32_t k = 0;
    if (i < 20) {
      f = (b & c) | (~b & d);
      k = 0x5a827999u;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ed9eba1u;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8f1bbcdcu;
    } else {
      f = b ^ c ^ d;
      k = 0xca62c1d6u;
    }
    const uint32_t temp = rotate_left(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = rotate_left(b, 30);
    b = a;
    a = temp;
  }
  ctx.h[0] += a;
  ctx.h[1] += b;
  ctx.h[2] += c;
  ctx.h[3] += d;
  ctx.h[4] += e;
}

void sha1_update(sha1_ctx& ctx, const uint8_t* data, std::size_t len) {
  ctx.total_bytes += len;
  while (len > 0) {
    const std::size_t take = std::min<std::size_t>(len, 64 - ctx.used);
    std::memcpy(ctx.buffer + ctx.used, data, take);
    ctx.used += take;
    data += take;
    len -= take;
    if (ctx.used == 64) {
      sha1_block(ctx, ctx.buffer);
      ctx.used = 0;
    }
  }
}

std::string sha1_digest(const std::string& message) {
  sha1_ctx ctx;
  sha1_update(ctx, reinterpret_cast<const uint8_t*>(message.data()),
              message.size());
  const uint64_t bit_length = ctx.total_bytes * 8ull;
  const uint8_t pad = 0x80;
  sha1_update(ctx, &pad, 1);
  const uint8_t zero = 0;
  while (ctx.used != 56) sha1_update(ctx, &zero, 1);
  uint8_t length[8];
  for (int i = 0; i < 8; ++i) {
    length[7 - i] = static_cast<uint8_t>((bit_length >> (i * 8)) & 0xffu);
  }
  sha1_update(ctx, length, sizeof(length));
  std::string out(20, '\0');
  for (int i = 0; i < 5; ++i) {
    out[i * 4] = static_cast<char>((ctx.h[i] >> 24) & 0xffu);
    out[i * 4 + 1] = static_cast<char>((ctx.h[i] >> 16) & 0xffu);
    out[i * 4 + 2] = static_cast<char>((ctx.h[i] >> 8) & 0xffu);
    out[i * 4 + 3] = static_cast<char>(ctx.h[i] & 0xffu);
  }
  return out;
}

std::string hmac_sha1(const std::string& key, const std::string& message) {
  std::string block = key.size() > 64 ? sha1_digest(key) : key;
  block.resize(64, '\0');
  std::string inner(64, '\0');
  std::string outer(64, '\0');
  for (std::size_t i = 0; i < 64; ++i) {
    inner[i] = static_cast<char>(static_cast<unsigned char>(block[i]) ^ 0x36);
    outer[i] = static_cast<char>(static_cast<unsigned char>(block[i]) ^ 0x5c);
  }
  return sha1_digest(outer + sha1_digest(inner + message));
}

bool constant_time_equal(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i]) ^
            static_cast<unsigned char>(b[i]);
  }
  return diff == 0;
}

std::string code_at(const std::string& secret, uint64_t counter) {
  std::string message(8, '\0');
  for (int i = 7; i >= 0; --i) {
    message[static_cast<std::size_t>(i)] = static_cast<char>(counter & 0xffu);
    counter >>= 8;
  }
  const std::string mac = hmac_sha1(secret, message);
  if (mac.size() < 20) return std::string();
  const int offset = static_cast<unsigned char>(mac[19]) & 0x0f;
  const uint32_t binary =
      ((static_cast<uint32_t>(static_cast<unsigned char>(mac[offset]))) &
       0x7fu)
          << 24 |
      (static_cast<uint32_t>(static_cast<unsigned char>(mac[offset + 1])))
          << 16 |
      (static_cast<uint32_t>(static_cast<unsigned char>(mac[offset + 2])))
          << 8 |
      static_cast<uint32_t>(static_cast<unsigned char>(mac[offset + 3]));
  const uint32_t value = binary % 1000000u;
  std::string out(6, '0');
  uint32_t rest = value;
  for (int i = 5; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] =
        static_cast<char>('0' + (rest % 10u));
    rest /= 10u;
  }
  return out;
}

int base32_value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a';
  if (c >= '2' && c <= '7') return c - '2' + 26;
  return -1;
}

std::string to_hex(const std::string& bytes) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (unsigned char c : bytes) {
    out.push_back(kHex[c >> 4]);
    out.push_back(kHex[c & 0x0f]);
  }
  return out;
}

bool from_hex(const std::string& text, std::string& bytes) {
  if (text.size() % 2 != 0) return false;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string out;
  out.reserve(text.size() / 2);
  for (std::size_t i = 0; i < text.size(); i += 2) {
    const int high = nibble(text[i]);
    const int low = nibble(text[i + 1]);
    if (high < 0 || low < 0) return false;
    out.push_back(static_cast<char>((high << 4) | low));
  }
  bytes = std::move(out);
  return true;
}

std::string derive_key(const std::string& key, const char* label) {
  return hmac_sha1(key, std::string(label));
}

std::string xor_keystream(const std::string& stream_key,
                          const std::string& nonce,
                          const std::string& data) {
  std::string out(data.size(), '\0');
  uint32_t block = 0;
  for (std::size_t offset = 0; offset < data.size(); offset += 20) {
    std::string message = "molink-totp-ctr";
    message.append(nonce);
    message.push_back(static_cast<char>((block >> 24) & 0xffu));
    message.push_back(static_cast<char>((block >> 16) & 0xffu));
    message.push_back(static_cast<char>((block >> 8) & 0xffu));
    message.push_back(static_cast<char>(block & 0xffu));
    const std::string pad = hmac_sha1(stream_key, message);
    const std::size_t take =
        std::min<std::size_t>(20, data.size() - offset);
    for (std::size_t i = 0; i < take; ++i) {
      out[offset + i] = static_cast<char>(
          static_cast<unsigned char>(data[offset + i]) ^
          static_cast<unsigned char>(pad[i]));
    }
    ++block;
  }
  return out;
}

std::string percent_encode(const std::string& value) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : value) {
    const bool unreserved = (c >= 'A' && c <= 'Z') ||
                            (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                            c == '_' || c == '~';
    if (unreserved) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0f]);
    }
  }
  return out;
}

}  // namespace

std::string random_bytes(std::size_t count) {
  std::string out(count, '\0');
  std::size_t filled = 0;
#if !defined(_WIN32)
  if (FILE* source = std::fopen("/dev/urandom", "rb")) {
    filled = std::fread(&out[0], 1, count, source);
    std::fclose(source);
    if (filled > count) filled = count;
  }
#endif
  if (filled < count) {
    std::random_device device;
    for (; filled < count; ++filled) {
      out[filled] = static_cast<char>(device() & 0xffu);
    }
  }
  return out;
}

std::string base32_encode(const std::string& raw) {
  static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
  std::string out;
  out.reserve((raw.size() * 8 + 4) / 5);
  uint32_t buffer = 0;
  int bits = 0;
  for (unsigned char c : raw) {
    buffer = (buffer << 8) | c;
    bits += 8;
    while (bits >= 5) {
      bits -= 5;
      out.push_back(kAlphabet[(buffer >> bits) & 0x1fu]);
    }
  }
  if (bits > 0) {
    out.push_back(kAlphabet[(buffer << (5 - bits)) & 0x1fu]);
  }
  return out;
}

bool base32_decode(const std::string& text, std::string& raw) {
  std::string out;
  out.reserve(text.size() * 5 / 8);
  uint32_t buffer = 0;
  int bits = 0;
  for (char c : text) {
    if (c == '=' || c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
        c == '-') {
      continue;
    }
    const int value = base32_value(c);
    if (value < 0) return false;
    buffer = (buffer << 5) | static_cast<uint32_t>(value);
    bits += 5;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((buffer >> bits) & 0xffu));
    }
  }
  raw = std::move(out);
  return true;
}

std::string generate_secret() { return random_bytes(kSecretBytes); }

std::string code_for(const std::string& secret, uint64_t unix_seconds) {
  return code_at(secret, unix_seconds / kStepSeconds);
}

bool verify(const std::string& secret, const std::string& code,
            uint64_t unix_seconds, int window) {
  std::string normalized;
  for (char c : code) {
    if (c == ' ' || c == '\t') continue;
    if (c < '0' || c > '9') return false;
    normalized.push_back(c);
  }
  if (normalized.size() != static_cast<std::size_t>(kDigits)) return false;
  if (window < 0) window = 0;
  const uint64_t counter = unix_seconds / kStepSeconds;
  bool match = false;
  for (int delta = -window; delta <= window; ++delta) {
    const int64_t candidate = static_cast<int64_t>(counter) + delta;
    if (candidate < 0) continue;
    if (constant_time_equal(
            code_at(secret, static_cast<uint64_t>(candidate)), normalized)) {
      match = true;
    }
  }
  return match;
}

std::string otpauth_uri(const std::string& secret_base32,
                        const std::string& account,
                        const std::string& issuer) {
  const std::string label =
      percent_encode(issuer) + ":" + percent_encode(account);
  return "otpauth://totp/" + label + "?secret=" + secret_base32 +
         "&issuer=" + percent_encode(issuer) + "&algorithm=SHA1&digits=6" +
         "&period=30";
}

std::string seal(const std::string& plaintext, const std::string& key) {
  const std::string nonce = random_bytes(16);
  const std::string stream_key = derive_key(key, "molink-totp-enc");
  const std::string mac_key = derive_key(key, "molink-totp-mac");
  const std::string ciphertext = xor_keystream(stream_key, nonce, plaintext);
  const std::string tag =
      hmac_sha1(mac_key, std::string("molink-totp-tag") + nonce + ciphertext);
  return to_hex(nonce + ciphertext + tag);
}

bool open(const std::string& sealed, const std::string& key,
          std::string& plaintext) {
  std::string bytes;
  if (!from_hex(sealed, bytes) || bytes.size() < 16 + 20) return false;
  const std::string nonce = bytes.substr(0, 16);
  const std::string tag = bytes.substr(bytes.size() - 20);
  const std::string ciphertext = bytes.substr(16, bytes.size() - 16 - 20);
  const std::string mac_key = derive_key(key, "molink-totp-mac");
  const std::string expected =
      hmac_sha1(mac_key, std::string("molink-totp-tag") + nonce + ciphertext);
  if (!constant_time_equal(tag, expected)) return false;
  const std::string stream_key = derive_key(key, "molink-totp-enc");
  plaintext = xor_keystream(stream_key, nonce, ciphertext);
  return true;
}

}  // namespace totp
}  // namespace molink
