#include "clipboard/clipboard.h"

#if defined(__APPLE__)

#import <AppKit/AppKit.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace molink {
namespace {

std::string utf8(NSString* text) {
  if (text == nil) return std::string();
  const char* value = [text UTF8String];
  return value != nullptr ? std::string(value) : std::string();
}

class macos_clipboard final : public clipboard_service {
 public:
  ~macos_clipboard() override {
    running_.store(false);
    if (watcher_.joinable()) watcher_.join();
  }

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
    // NSPasteboard has no owner API. The change counter is the witness, just
    // as GetClipboardOwner is on Windows: the next counter value is ours.
    self_change_count_.store([pasteboard changeCount]);
  }

  void request_text(text_callback callback) override {
    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    NSString* value = [pasteboard stringForType:NSPasteboardTypeString];
    if (callback) callback(utf8(value));
  }

  void request_files(files_callback callback) override {
    if (callback) callback(read_files());
  }

  /// NSPasteboard has no notification callback; poll its change counter the
  /// way the Windows backend polls GetClipboardSequenceNumber.
  void watch(text_watcher callback) override {
    std::lock_guard<std::mutex> lock(watch_mutex_);
    text_watcher_ = std::move(callback);
  }

  void watch_files(files_watcher callback) override {
    if (callback == nullptr) return;
    {
      std::lock_guard<std::mutex> lock(watch_mutex_);
      files_callback_ = std::move(callback);
    }
    if (running_.exchange(true)) return;
    watcher_ = std::thread([this] {
      @autoreleasepool {
        NSInteger last = [[NSPasteboard generalPasteboard] changeCount];
        while (running_.load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(800));
          if (!running_.load()) break;
          @autoreleasepool {
            NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
            const NSInteger current = [pasteboard changeCount];
            if (current == last) continue;
            last = current;
            const clipboard_owner owner =
                current == self_change_count_.load()
                    ? clipboard_owner::self
                    : clipboard_owner::external;
            NSString* text = [pasteboard stringForType:NSPasteboardTypeString];
            if (text != nil) {
              std::lock_guard<std::mutex> lock(watch_mutex_);
              std::string value = utf8(text);
              // NSPasteboard.changeCount already proves this is a new copy.
              // Do not compare the content: copying the same string twice is
              // two copies, and the viewer must see the second one.
              if (!value.empty()) {
                last_files_.clear();
                if (text_watcher_) text_watcher_(value, owner);
              }
            }
            // Text and files can both be on the pasteboard; report each when
            // it changed instead of letting text hide a Finder selection.
            std::vector<clipboard_file> files = read_files();
            if (files.empty()) {
              std::lock_guard<std::mutex> lock(watch_mutex_);
              last_files_.clear();
              continue;
            }
            std::string signature;
            for (const clipboard_file& file : files) {
              signature += file.path;
              signature += '\n';
            }
            std::lock_guard<std::mutex> lock(watch_mutex_);
            if (signature == last_files_) continue;
            last_files_ = signature;
            if (files_callback_) {
              files_callback_(std::move(files), owner);
            }
          }
        }
      }
    });
  }

  /// Put real file URLs on the pasteboard. Finder's paste reads these and
  /// shows its normal per-file progress as the files are copied.
  bool set_files(const std::vector<std::string>& paths) override {
    if (paths.empty()) return false;
    NSMutableArray<NSURL*>* urls = [NSMutableArray arrayWithCapacity:paths.size()];
    for (const std::string& path : paths) {
      if (path.empty()) continue;
      NSString* value = [NSString stringWithUTF8String:path.c_str()];
      if (value == nil) continue;
      NSURL* url = [NSURL fileURLWithPath:value isDirectory:NO];
      if (url != nil) [urls addObject:url];
    }
    if ([urls count] == 0) return false;
    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    [pasteboard clearContents];
    const BOOL written = [pasteboard writeObjects:urls] == YES;
    if (written) self_change_count_.store([pasteboard changeCount]);
    return written;
  }

 private:
  static std::vector<clipboard_file> read_files() {
    std::vector<clipboard_file> files;
    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    NSDictionary* options = @{
      NSPasteboardURLReadingFileURLsOnlyKey : @YES,
    };
    NSArray<NSURL*>* urls =
        [pasteboard readObjectsForClasses:@[ [NSURL class] ]
                                  options:options];
    if (urls == nil) return files;
    NSFileManager* manager = [NSFileManager defaultManager];
    for (NSURL* url in urls) {
      if (![url isFileURL]) continue;
      NSString* path = [url path];
      if (path == nil) continue;
      NSDictionary* attributes =
          [manager attributesOfItemAtPath:path error:nil];
      if (attributes == nil) continue;
      // Finder can put folders on the pasteboard; the clipboard file list is
      // regular files only (folders stay in the files panel).
      if (![[attributes objectForKey:NSFileType] isEqual:NSFileTypeRegular]) {
        continue;
      }
      clipboard_file file;
      file.path = utf8(path);
      file.name = utf8([path lastPathComponent]);
      NSNumber* size = attributes[NSFileSize];
      if (size != nil) file.size = [size unsignedLongLongValue];
      bool duplicate = false;
      for (const clipboard_file& existing : files) {
        if (existing.path == file.path) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) files.push_back(std::move(file));
    }
    return files;
  }

  std::mutex watch_mutex_;
  text_watcher text_watcher_;
  files_watcher files_callback_;
  std::string last_files_;
  std::atomic<bool> running_{false};
  /// changeCount after this process last wrote the pasteboard; the watcher
  /// uses it as the "owner is us" test.
  std::atomic<NSInteger> self_change_count_{0};
  std::thread watcher_;
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
