#pragma once

#include <cstdint>
#include <vector>

namespace molink {

/// PNG -> CF_DIB payload (BITMAPINFOHEADER followed by 32-bit BGRA rows,
/// bottom-up). Returns false when the PNG cannot be decoded or its dimensions
/// are absurd.
bool clipboard_png_to_dib(const std::vector<uint8_t>& png,
                          std::vector<uint8_t>& dib);

/// CF_DIB payload -> PNG. Handles the 24- and 32-bit BI_RGB / BI_BITFIELDS
/// forms every screenshot tool and browser writes; other depths return false
/// (the host then falls back to the text clipboard).
bool clipboard_dib_to_png(const std::vector<uint8_t>& dib,
                          std::vector<uint8_t>& png);

}  // namespace molink
