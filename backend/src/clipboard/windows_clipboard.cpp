#include "clipboard/clipboard.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>
#include <string>

namespace molink {
namespace {

std::wstring to_wide(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      wide.data(), needed);
  return wide;
}

std::string to_utf8(const wchar_t* text) {
  if (text == nullptr) return std::string();
  const int needed =
      WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 1) return std::string();
  std::string out(static_cast<std::size_t>(needed - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed - 1, nullptr,
                      nullptr);
  return out;
}

class windows_clipboard final : public clipboard_service {
 public:
  bool available() const override { return true; }
  std::string backend() const override { return "win32-clipboard"; }

  void set_text(std::string text) override {
    constexpr std::size_t kMaxText = 192 * 1024;
    if (text.size() > kMaxText) text.resize(kMaxText);
    const std::wstring wide = to_wide(text);

    for (int attempt = 0; attempt < 5; ++attempt) {
      if (!OpenClipboard(nullptr)) {
        Sleep(10);
        continue;
      }
      EmptyClipboard();
      const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
      HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
      if (memory != nullptr) {
        void* target = GlobalLock(memory);
        if (target != nullptr) {
          std::memcpy(target, wide.c_str(), bytes);
          GlobalUnlock(memory);
          if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
            GlobalFree(memory);
          }
        } else {
          GlobalFree(memory);
        }
      }
      CloseClipboard();
      return;
    }
  }

  void request_text(text_callback callback) override {
    std::string text;
    if (OpenClipboard(nullptr)) {
      if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE data = GetClipboardData(CF_UNICODETEXT);
        if (data != nullptr) {
          const wchar_t* wide = static_cast<const wchar_t*>(GlobalLock(data));
          if (wide != nullptr) {
            text = to_utf8(wide);
            GlobalUnlock(data);
          }
        }
      }
      CloseClipboard();
    }
    if (callback) callback(std::move(text));
  }
};

}  // namespace

std::unique_ptr<clipboard_service> make_windows_clipboard(std::string&) {
  return std::unique_ptr<clipboard_service>(new windows_clipboard());
}

}  // namespace molink

#else  // !_WIN32

namespace molink {
std::unique_ptr<clipboard_service> make_windows_clipboard(std::string& err) {
  err = "built without Windows clipboard support";
  return nullptr;
}
}  // namespace molink

#endif
