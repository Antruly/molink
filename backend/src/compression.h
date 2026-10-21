#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace molink {

/// True when the build has zlib and tile compression is possible.
bool compression_available();

/// zlib-wrapped deflate (what `DecompressionStream("deflate")` expects).
/// Level 1: screen content compresses well and the capture thread must not
/// spend long on a keyframe.
bool deflate_buffer(const uint8_t* data, std::size_t len,
                    std::vector<uint8_t>& out, std::string& err);

}  // namespace molink
