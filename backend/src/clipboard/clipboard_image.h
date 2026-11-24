#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "clipboard/clipboard_route.h"

namespace molink {

/// Largest PNG the clipboard channel accepts, measured after compression.
/// A screenshot is normally well under this; a huge raw DIB is rejected
/// instead of being expanded into the session's file channel.
inline constexpr std::size_t kClipboardImageMaxBytes =
    8u * 1024u * 1024u;

/// The virtual file name an image travels under. The bytes are PNG in every
/// direction; the receiving side decides whether to write CF_DIB, image/png or
/// NSPasteboardTypePNG.
inline constexpr const char* kClipboardImageName = "clipboard.png";

/// One platform clipboard image, normalised to PNG.
///
/// `source_bytes` is the size of the platform representation (PNG or DIB), so
/// an over-limit copy can be logged without keeping the bytes. `too_large` is
/// set instead of silently reporting "no image": the supervisor distinguishes
/// "the clipboard holds no image" from "there is one, but we refuse it".
struct clipboard_image {
  std::vector<uint8_t> png;
  uint64_t source_bytes = 0;
  bool too_large = false;

  bool empty() const { return png.empty(); }
};

/// Applies the 8 MiB cap to already-encoded PNG bytes.
clipboard_image clipboard_image_clamped(std::vector<uint8_t> png);

/// FNV-1a over the PNG bytes, lower-case hex. Used for watcher de-duplication
/// and the in-memory offer; it is not a security boundary.
std::string clipboard_image_hash(const std::vector<uint8_t>& bytes);

/// What the host supervisor should do with one image watcher event.
///
/// A value this process wrote (a viewer's image applied to the local
/// clipboard) must never travel back out as a fresh local copy; everything
/// else, including an owner the platform cannot name, is a normal copy.
enum class clipboard_image_disposition {
  announce,
  drop_self,
  drop_empty,
  drop_too_large,
};

clipboard_image_disposition clipboard_image_disposition_for(
    clipboard_owner owner, const clipboard_image& image);

}  // namespace molink
