#include "compression.h"

#if MOLINK_HAVE_ZLIB
#include <zlib.h>
#endif

namespace molink {

bool compression_available() {
#if MOLINK_HAVE_ZLIB
  return true;
#else
  return false;
#endif
}

bool deflate_buffer(const uint8_t* data, std::size_t len, int level,
                    std::vector<uint8_t>& out, std::string& err) {
#if MOLINK_HAVE_ZLIB
  if (len == 0) {
    out.clear();
    return true;
  }
  if (level < 1) level = 1;
  if (level > 9) level = 9;
  if (len > static_cast<std::size_t>(uLongf(-1))) {
    err = "tile is too large for zlib";
    return false;
  }

  // compressBound is an upper bound for deflate; +64 covers the zlib wrapper
  // on builds where compressBound is conservative.
  uLongf bound = compressBound(static_cast<uLong>(len));
  out.resize(static_cast<std::size_t>(bound) + 64);
  const int rc = compress2(out.data(), &bound, data, static_cast<uLong>(len),
                           level);
  if (rc != Z_OK) {
    err = "zlib compress2 failed with " + std::to_string(rc);
    out.clear();
    return false;
  }
  out.resize(static_cast<std::size_t>(bound));
  return true;
#else
  (void)data;
  (void)len;
  (void)out;
  err = "built without zlib";
  return false;
#endif
}

}  // namespace molink
