#include "clipboard/clipboard_image.h"

#include <cstdio>
#include <utility>

namespace molink {

clipboard_image clipboard_image_clamped(std::vector<uint8_t> png) {
  clipboard_image image;
  image.source_bytes = png.size();
  if (png.size() > kClipboardImageMaxBytes) {
    image.too_large = true;
    // Do not keep the oversized bytes: the caller only needs the size for its
    // diagnostic, and the text clipboard (if any) still syncs separately.
    png.clear();
  }
  image.png = std::move(png);
  return image;
}

std::string clipboard_image_hash(const std::vector<uint8_t>& bytes) {
  uint64_t hash = 1469598103934665603ull;
  for (const unsigned char byte : bytes) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  char buffer[17] = {};
  std::snprintf(buffer, sizeof(buffer), "%016llx",
                static_cast<unsigned long long>(hash));
  return buffer;
}

clipboard_image_disposition clipboard_image_disposition_for(
    clipboard_owner owner, const clipboard_image& image) {
  if (owner == clipboard_owner::self) {
    return clipboard_image_disposition::drop_self;
  }
  if (image.too_large) return clipboard_image_disposition::drop_too_large;
  if (image.empty()) return clipboard_image_disposition::drop_empty;
  return clipboard_image_disposition::announce;
}

}  // namespace molink
