#include "security/cn_ranges.h"

#include <cstddef>

namespace molink {
namespace {

struct cn_range {
  std::uint32_t start;
  std::uint32_t end;
};

#include "security/cn_cidr_generated.inc"

}  // namespace

bool cn_ipv4_contains(std::uint32_t address) {
  std::size_t low = 0;
  std::size_t high = kCnRangeCount;
  while (low < high) {
    const std::size_t mid = low + (high - low) / 2;
    if (address < kCnRanges[mid].start) {
      high = mid;
    } else if (address > kCnRanges[mid].end) {
      low = mid + 1;
    } else {
      return true;
    }
  }
  return false;
}

}  // namespace molink
