#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace molink {

/// Minimal SHA-256 (FIPS 180-4). A resumed upload carries an optional digest
/// of the prefix the viewer already sent; the host hashes the matching prefix
/// of its retained `.part` file and only resumes when the two agree.
///
/// Header-only so the file-transfer tests (which deliberately link no crypto
/// library) can compute the same digest.
class sha256 {
 public:
  sha256() { reset(); }

  void reset() {
    static constexpr uint32_t kInitial[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    std::memcpy(state_, kInitial, sizeof(state_));
    total_ = 0;
    buffer_len_ = 0;
    finalized_ = false;
  }

  void update(const void* data, std::size_t len) {
    total_ += static_cast<uint64_t>(len);
    block_update(data, len);
  }

  /// Finalises the hash and returns the lowercase hex digest. Calling it a
  /// second time returns the same digest.
  std::string hex_digest() {
    if (!finalized_) {
      const uint64_t bits = total_ * 8;
      const uint8_t pad = 0x80;
      block_update(&pad, 1);
      const uint8_t zero = 0;
      while (buffer_len_ != 56) block_update(&zero, 1);
      uint8_t size[8];
      for (int index = 0; index < 8; ++index) {
        size[index] =
            static_cast<uint8_t>((bits >> (56 - index * 8)) & 0xffu);
      }
      block_update(size, sizeof(size));
      finalized_ = true;
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int index = 0; index < 8; ++index) {
      for (int shift = 28; shift >= 0; shift -= 4) {
        out.push_back(kHex[(state_[index] >> shift) & 0xfu]);
      }
    }
    return out;
  }

 private:
  static uint32_t rotr(uint32_t value, uint32_t count) {
    return (value >> count) | (value << (32 - count));
  }

  void block_update(const void* data, std::size_t len) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    while (len > 0) {
      std::size_t take = 64 - buffer_len_;
      if (take > len) take = len;
      std::memcpy(buffer_ + buffer_len_, bytes, take);
      buffer_len_ += take;
      bytes += take;
      len -= take;
      if (buffer_len_ == 64) {
        transform(buffer_);
        buffer_len_ = 0;
      }
    }
  }

  void transform(const uint8_t* block) {
    static constexpr uint32_t kRound[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
        0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
        0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
        0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
        0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
        0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
        0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
        0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
        0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
        0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
    uint32_t words[64];
    for (int index = 0; index < 16; ++index) {
      words[index] = (static_cast<uint32_t>(block[index * 4]) << 24) |
                     (static_cast<uint32_t>(block[index * 4 + 1]) << 16) |
                     (static_cast<uint32_t>(block[index * 4 + 2]) << 8) |
                     static_cast<uint32_t>(block[index * 4 + 3]);
    }
    for (int index = 16; index < 64; ++index) {
      const uint32_t s0 = rotr(words[index - 15], 7) ^
                          rotr(words[index - 15], 18) ^
                          (words[index - 15] >> 3);
      const uint32_t s1 = rotr(words[index - 2], 17) ^
                          rotr(words[index - 2], 19) ^
                          (words[index - 2] >> 10);
      words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }
    uint32_t a = state_[0];
    uint32_t b = state_[1];
    uint32_t c = state_[2];
    uint32_t d = state_[3];
    uint32_t e = state_[4];
    uint32_t f = state_[5];
    uint32_t g = state_[6];
    uint32_t h = state_[7];
    for (int index = 0; index < 64; ++index) {
      const uint32_t s1 =
          rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const uint32_t choice = (e & f) ^ (~e & g);
      const uint32_t temp1 = h + s1 + choice + kRound[index] + words[index];
      const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t temp2 = s0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  uint32_t state_[8];
  uint64_t total_ = 0;
  uint8_t buffer_[64] = {};
  std::size_t buffer_len_ = 0;
  bool finalized_ = false;
};

inline std::string sha256_hex(const void* data, std::size_t len) {
  sha256 hash;
  hash.update(data, len);
  return hash.hex_digest();
}

inline std::string sha256_hex(const std::string& text) {
  return sha256_hex(text.data(), text.size());
}

}  // namespace molink
