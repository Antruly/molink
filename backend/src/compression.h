#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace molink {

/// True when the build has zlib and tile compression is possible.
bool compression_available();

/// zlib-wrapped deflate (what `DecompressionStream("deflate")` expects).
/// @p level is 1..9. The capture thread picks a fast level for keyframes and
/// bursts, and a slower one for small delta frames where the CPU is idle.
bool deflate_buffer(const uint8_t* data, std::size_t len, int level,
                    std::vector<uint8_t>& out, std::string& err);

}  // namespace molink
