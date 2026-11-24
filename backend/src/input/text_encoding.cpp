#include "input/text_encoding.h"

namespace molink {

std::uint32_t x11_unicode_keysym(std::uint32_t codepoint) {
  if (codepoint == '\n' || codepoint == '\r') return kX11ReturnKeysym;
  if (codepoint == '\t') return kX11TabKeysym;
  if (codepoint == 0x08) return kX11BackspaceKeysym;
  if (codepoint == 0) return 0;
  // Control characters other than the three handled above have no printable
  // X keysym; the injector skips them rather than typing a replacement.
  if (codepoint < 0x20 || (codepoint >= 0x7f && codepoint < 0xa0)) return 0;
  if (codepoint <= 0xff) return codepoint;
  if (codepoint > 0x10ffff) return 0;
  return 0x01000000u | codepoint;
}

std::u16string utf16_units(const std::vector<std::uint32_t>& codepoints) {
  std::u16string out;
  out.reserve(codepoints.size());
  for (const std::uint32_t codepoint : codepoints) {
    if (codepoint <= 0xffff) {
      out.push_back(static_cast<char16_t>(codepoint));
    } else if (codepoint <= 0x10ffff) {
      const std::uint32_t rest = codepoint - 0x10000;
      out.push_back(static_cast<char16_t>(0xd800 + (rest >> 10)));
      out.push_back(static_cast<char16_t>(0xdc00 + (rest & 0x3ff)));
    }
  }
  return out;
}

}  // namespace molink
