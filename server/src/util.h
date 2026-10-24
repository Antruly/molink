#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace molink {

uint64_t epoch_ms();
uint64_t monotonic_ms();

/// Cryptographically secure random bytes. Empty on failure.
std::string random_bytes(std::size_t count);
/// `count` random bytes as lowercase hex (2 * count characters).
std::string random_hex(std::size_t count);
std::string sha256_hex(const std::string& data);

bool constant_time_equals(const std::string& left, const std::string& right);

std::string trim(std::string value);
std::string lowercase_ascii(std::string value);
std::vector<std::string> split(const std::string& value, char separator);
bool has_whitespace_or_control(const std::string& value);
std::string url_encode(const std::string& value);

/// Clamp a parsed integer without undefined behaviour on huge values.
int clamp_int(long long value, int minimum, int maximum, int fallback);

}  // namespace molink
