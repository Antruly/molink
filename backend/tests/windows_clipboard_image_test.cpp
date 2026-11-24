// CF_DIB <-> PNG conversion behind the Windows image clipboard. It never
// touches the real clipboard, so it runs on every Windows build (including
// CI) without replacing the developer's clipboard.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "clipboard/clipboard_image_win.h"

namespace {

using namespace molink;

int checks = 0;
int failures = 0;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

void put_u32(std::vector<uint8_t>& bytes, std::size_t offset, uint32_t value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void put_i32(std::vector<uint8_t>& bytes, std::size_t offset, int32_t value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

/// A 2x2, 24-bit, bottom-up BI_RGB DIB. Rows are padded to four bytes (2*3
/// pixels = 6 bytes, stride 8).
std::vector<uint8_t> make_dib() {
  const std::size_t stride = 8;
  std::vector<uint8_t> dib(40 + stride * 2, 0);
  put_u32(dib, 0, 40);       // biSize
  put_i32(dib, 4, 2);        // biWidth
  put_i32(dib, 8, 2);        // biHeight (bottom-up)
  dib[12] = 1;               // biPlanes
  dib[13] = 0;
  dib[14] = 24;              // biBitCount
  dib[15] = 0;
  put_u32(dib, 16, 0);       // BI_RGB
  put_u32(dib, 20, stride * 2);  // biSizeImage

  // Bottom row (B, G, R): red and green.
  uint8_t* bottom = dib.data() + 40;
  bottom[0] = 0; bottom[1] = 0; bottom[2] = 255;   // red
  bottom[3] = 0; bottom[4] = 255; bottom[5] = 0;   // green
  // Top row: blue and white.
  uint8_t* top = dib.data() + 40 + stride;
  top[0] = 255; top[1] = 0; top[2] = 0;            // blue
  top[3] = 255; top[4] = 255; top[5] = 255;        // white
  return dib;
}

void test_dib_to_png_and_back() {
  const std::vector<uint8_t> dib = make_dib();
  std::vector<uint8_t> png;
  expect(clipboard_dib_to_png(dib, png), "a 24-bit BI_RGB DIB encodes");
  expect(png.size() >= 8, "the encoded value is a PNG");
  static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a,
                                       '\n'};
  expect(png.size() >= sizeof(signature) &&
             std::memcmp(png.data(), signature, sizeof(signature)) == 0,
         "the PNG signature is present");

  std::vector<uint8_t> roundtrip;
  expect(clipboard_png_to_dib(png, roundtrip),
         "the PNG decodes back to CF_DIB");
  expect(roundtrip.size() == 40 + 2 * 2 * 4,
         "the result is a 32-bit 2x2 bottom-up DIB");
  expect(roundtrip[14] == 32, "the decoded DIB is 32 bits per pixel");
  int32_t height = 0;
  std::memcpy(&height, roundtrip.data() + 8, sizeof(height));
  expect(height == 2, "the decoded DIB is bottom-up");

  // Bottom row: red, green. Top row: blue, white. BGRA order.
  const uint8_t* bottom = roundtrip.data() + 40;
  const uint8_t* top = bottom + 2 * 4;
  expect(bottom[0] == 0 && bottom[1] == 0 && bottom[2] == 255,
         "the bottom-left pixel is still red");
  expect(bottom[4] == 0 && bottom[5] == 255 && bottom[6] == 0,
         "the bottom-right pixel is still green");
  expect(top[0] == 255 && top[1] == 0 && top[2] == 0,
         "the top-left pixel is still blue");
  expect(top[4] == 255 && top[5] == 255 && top[6] == 255,
         "the top-right pixel is still white");
  expect(bottom[3] == 255 && top[3] == 255,
         "BI_RGB 32-bit pixels are opaque, not transparent");
}

void test_bad_input_is_rejected() {
  std::vector<uint8_t> png;
  expect(!clipboard_dib_to_png({}, png), "an empty DIB is rejected");
  expect(!clipboard_dib_to_png({1, 2, 3}, png), "a truncated DIB is rejected");

  std::vector<uint8_t> truncated(40, 0);
  put_u32(truncated, 0, 40);
  put_i32(truncated, 4, 100);
  put_i32(truncated, 8, 100);
  truncated[12] = 1;
  truncated[14] = 24;
  expect(!clipboard_dib_to_png(truncated, png),
         "a DIB whose rows are missing is rejected");

  std::vector<uint8_t> dib;
  expect(!clipboard_png_to_dib({}, dib), "empty PNG bytes are rejected");
  expect(!clipboard_png_to_dib({0x89, 'P', 'N', 'G'}, dib),
         "a truncated PNG is rejected");
}

}  // namespace

int main() {
  test_dib_to_png_and_back();
  test_bad_input_is_rejected();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
