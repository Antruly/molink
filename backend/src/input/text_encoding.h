#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace molink {

/// X11 keysyms for the control characters that have a dedicated key. Kept as
/// constants so the mapping test does not need the X11 headers.
constexpr std::uint32_t kX11ReturnKeysym = 0xff0d;
constexpr std::uint32_t kX11TabKeysym = 0xff09;
constexpr std::uint32_t kX11BackspaceKeysym = 0xff08;

/// X keysym that types @p codepoint on an X11 host.
///
/// X11 encodes Unicode above Latin-1 as `0x01000000 + codepoint`; Latin-1
/// maps to the code point itself. Newline/tab/backspace keep their control
/// keysyms so a remote editor sees the same key it would see from a keyboard.
/// Returns zero for a code point that cannot be represented.
std::uint32_t x11_unicode_keysym(std::uint32_t codepoint);

/// UTF-16 code units for Windows `KEYEVENTF_UNICODE`, with surrogate pairs for
/// code points above the BMP. This is the payload the Windows injector sends,
/// so the test can pin the exact units for Chinese text and emoji.
std::u16string utf16_units(const std::vector<std::uint32_t>& codepoints);

}  // namespace molink
