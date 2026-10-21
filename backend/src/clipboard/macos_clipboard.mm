#include "clipboard/clipboard.h"

#if defined(__APPLE__)

#import <AppKit/AppKit.h>

#include <string>

namespace molink {
namespace {

class macos_clipboard final : public clipboard_service {
 public:
  bool available() const override { return true; }
  std::string backend() const override { return "nspasteboard"; }

  void set_text(std::string text) override {
    constexpr std::size_t kMaxText = 192 * 1024;
    if (text.size() > kMaxText) text.resize(kMaxText);
    NSString* value = [NSString stringWithUTF8String:text.c_str()];
    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    [pasteboard clearContents];
    if (value != nil) {
      [pasteboard setString:value forType:NSPasteboardTypeString];
    }
  }

  void request_text(text_callback callback) override {
    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    NSString* value = [pasteboard stringForType:NSPasteboardTypeString];
    std::string text;
    if (value != nil) {
      const char* utf8 = [value UTF8String];
      if (utf8 != nullptr) text = utf8;
    }
    if (callback) callback(std::move(text));
  }
};

}  // namespace

std::unique_ptr<clipboard_service> make_macos_clipboard(std::string&) {
  return std::unique_ptr<clipboard_service>(new macos_clipboard());
}

}  // namespace molink

#else  // !__APPLE__

namespace molink {
std::unique_ptr<clipboard_service> make_macos_clipboard(std::string& err) {
  err = "built without macOS clipboard support";
  return nullptr;
}
}  // namespace molink

#endif
