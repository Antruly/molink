#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace molink {

/// True when this build links libjpeg-turbo.
bool jpeg_encode_available();

/// Encodes one BGRA picture as a baseline JPEG. Used for full-screen stream
/// frames on the viewer's core: a 1080p desktop frame is a few hundred
/// kilobytes instead of the four megabytes of raw RGB565, and the page decodes
/// it with the browser's native image decoder instead of inflating zlib.
bool jpeg_encode_bgra(const uint8_t* bgra, std::size_t stride, int width,
                      int height, int quality, std::vector<uint8_t>& out,
                      std::string& error);

}  // namespace molink
