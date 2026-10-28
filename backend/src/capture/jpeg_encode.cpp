#include "capture/jpeg_encode.h"

#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(MOLINK_HAVE_JPEG)
extern "C" {
#include <jpeglib.h>
}
#endif

namespace molink {

#if defined(MOLINK_HAVE_JPEG)

namespace {

struct jpeg_error_bridge {
  jpeg_error_mgr base;
  std::jmp_buf jump;
};

void on_jpeg_error(j_common_ptr info) {
  auto* bridge = reinterpret_cast<jpeg_error_bridge*>(info->err);
  char message[JMSG_LENGTH_MAX] = {};
  info->err->format_message(info, message);
  std::fprintf(stderr, "molink jpeg: %s\n", message);
  std::longjmp(bridge->jump, 1);
}

}  // namespace

bool jpeg_encode_available() { return true; }

bool jpeg_encode_bgra(const uint8_t* bgra, std::size_t stride, int width,
                      int height, int quality, std::vector<uint8_t>& out,
                      std::string& error) {
  out.clear();
  if (bgra == nullptr || width <= 0 || height <= 0) {
    error = "jpeg: invalid picture";
    return false;
  }
  if (stride < static_cast<std::size_t>(width) * 4) {
    error = "jpeg: stride is shorter than a row";
    return false;
  }
  jpeg_compress_struct encoder{};
  jpeg_error_bridge bridge{};
  encoder.err = jpeg_std_error(&bridge.base);
  bridge.base.error_exit = on_jpeg_error;
  unsigned char* memory = nullptr;
  unsigned long size = 0;
  if (setjmp(bridge.jump) != 0) {
    jpeg_destroy_compress(&encoder);
    if (memory != nullptr) std::free(memory);
    error = "jpeg: encode failed";
    return false;
  }
  jpeg_create_compress(&encoder);
  jpeg_mem_dest(&encoder, &memory, &size);
  encoder.image_width = static_cast<JDIMENSION>(width);
  encoder.image_height = static_cast<JDIMENSION>(height);
  encoder.input_components = 4;
  // libjpeg-turbo's "BGRX" input takes our BGRA rows without a conversion.
  encoder.in_color_space = JCS_EXT_BGRX;
  jpeg_set_defaults(&encoder);
  jpeg_set_quality(&encoder, quality < 1 ? 1 : (quality > 100 ? 100 : quality),
                   TRUE);
  // Screen content tolerates the faster DCT; it roughly halves encode time.
  encoder.dct_method = JDCT_IFAST;
  jpeg_start_compress(&encoder, TRUE);
  while (encoder.next_scanline < encoder.image_height) {
    JSAMPROW row = const_cast<JSAMPROW>(
        bgra + static_cast<std::size_t>(encoder.next_scanline) * stride);
    jpeg_write_scanlines(&encoder, &row, 1);
  }
  jpeg_finish_compress(&encoder);
  if (memory != nullptr && size > 0) {
    out.assign(memory, memory + size);
  }
  jpeg_destroy_compress(&encoder);
  if (memory != nullptr) std::free(memory);
  if (out.empty()) {
    error = "jpeg: produced no data";
    return false;
  }
  return true;
}

#else

bool jpeg_encode_available() { return false; }

bool jpeg_encode_bgra(const uint8_t*, std::size_t, int, int, int,
                      std::vector<uint8_t>& out, std::string& error) {
  out.clear();
  error = "this build has no jpeg encoder";
  return false;
}

#endif  // MOLINK_HAVE_JPEG

}  // namespace molink
