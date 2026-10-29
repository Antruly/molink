#include "clipboard/clipboard.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
  ~windows_clipboard() override { stop_watch(); }

  bool available() const override { return true; }
  std::string backend() const override { return "win32-clipboard"; }

  /// Polls GetClipboardSequenceNumber (cheap; no window needed) and reports
  /// the new text. This is what pushes what the host user copied to every
  /// viewer, so the clipboard behaves like RDP redirection in both
  /// directions.
  void watch(text_watcher callback) override {
    if (callback == nullptr) {
      stop_watch();
      return;
    }
    std::lock_guard<std::mutex> lock(watch_mutex_);
    watch_callback_ = std::move(callback);
    if (running_.exchange(true)) return;
    // Already holding watch_mutex_ here: locking it again is undefined.
    sequence_ = GetClipboardSequenceNumber();
    watcher_ = std::thread([this] {
      while (running_.load()) {
        Sleep(400);
        const DWORD sequence = GetClipboardSequenceNumber();
        {
          std::lock_guard<std::mutex> lock(watch_mutex_);
          if (sequence == sequence_) continue;
          sequence_ = sequence;
        }
        std::string text;
        request_text([&text](std::string value) { text = std::move(value); });
        if (!text.empty()) {
          std::lock_guard<std::mutex> lock(watch_mutex_);
          if (text == last_text_) continue;
          last_text_ = text;
          last_files_.clear();
          if (watch_callback_) watch_callback_(text);
          continue;
        }
        // No text: the clipboard may hold files (Explorer Ctrl+C).
        std::vector<clipboard_file> files;
        request_files([&files](std::vector<clipboard_file> value) {
          files = std::move(value);
        });
        if (files.empty()) continue;
        std::string signature;
        for (const clipboard_file& file : files) {
          signature += file.path;
          signature += '\n';
        }
        std::lock_guard<std::mutex> lock(watch_mutex_);
        if (signature == last_files_) continue;
        last_files_ = signature;
        last_text_.clear();
        if (files_callback_) files_callback_(files);
      }
    });
  }

  void watch_files(files_watcher callback) override {
    std::lock_guard<std::mutex> lock(watch_mutex_);
    files_callback_ = std::move(callback);
  }

  void stop_watch() {
    if (!running_.exchange(false)) return;
    if (watcher_.joinable()) watcher_.join();
    std::lock_guard<std::mutex> lock(watch_mutex_);
    watch_callback_ = nullptr;
  }

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
    // Clipboard managers and other apps hold the clipboard for short bursts;
    // one failing OpenClipboard used to drop a whole sync round.
    for (int attempt = 0; attempt < 5; ++attempt) {
      if (!OpenClipboard(nullptr)) {
        Sleep(10);
        continue;
      }
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
      break;
    }
    if (callback) callback(std::move(text));
  }

  void request_files(files_callback callback) override {
    std::vector<clipboard_file> files;
    bool opened = false;
    for (int attempt = 0; attempt < 5; ++attempt) {
      if (OpenClipboard(nullptr)) {
        opened = true;
        break;
      }
      Sleep(10);
    }
    if (opened) {
      if (IsClipboardFormatAvailable(CF_HDROP)) {
        HANDLE data = GetClipboardData(CF_HDROP);
        if (data != nullptr) {
          HDROP drop = static_cast<HDROP>(data);
          const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
          for (UINT i = 0; i < count; ++i) {
            const UINT length = DragQueryFileW(drop, i, nullptr, 0);
            if (length == 0) continue;
            std::wstring path(length + 1, L'\0');
            if (DragQueryFileW(drop, i, path.data(), length + 1) == 0) continue;
            path.resize(length);
            clipboard_file file;
            file.path = to_utf8(path.c_str());
            const std::filesystem::path fs_path(path);
            const std::wstring name = fs_path.filename().wstring();
            file.name = to_utf8(name.c_str());
            std::error_code ec;
            const auto size = std::filesystem::file_size(fs_path, ec);
            if (!ec) file.size = size;
            files.push_back(std::move(file));
          }
        }
      }
      CloseClipboard();
    }
    if (callback) callback(std::move(files));
  }

  bool set_files(const std::vector<std::string>& paths) override {
    std::vector<std::wstring> wide;
    wide.reserve(paths.size());
    for (const std::string& path : paths) {
      std::wstring converted = to_wide(path);
      if (!converted.empty()) wide.push_back(std::move(converted));
    }
    if (wide.empty()) return false;
    std::size_t characters = 1;  // final list terminator
    for (const std::wstring& path : wide) characters += path.size() + 1;
    const std::size_t bytes =
        sizeof(DROPFILES) + characters * sizeof(wchar_t);
    bool applied = false;
    for (int attempt = 0; attempt < 5; ++attempt) {
      if (!OpenClipboard(nullptr)) {
        Sleep(10);
        continue;
      }
      EmptyClipboard();
      HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
      if (memory != nullptr) {
        auto* drop = static_cast<DROPFILES*>(GlobalLock(memory));
        if (drop != nullptr) {
          ZeroMemory(drop, sizeof(DROPFILES));
          drop->pFiles = sizeof(DROPFILES);
          drop->fWide = TRUE;
          auto* cursor = reinterpret_cast<wchar_t*>(
              reinterpret_cast<BYTE*>(drop) + sizeof(DROPFILES));
          for (const std::wstring& path : wide) {
            std::memcpy(cursor, path.c_str(),
                        path.size() * sizeof(wchar_t));
            cursor += path.size();
            *cursor++ = L'\0';
          }
          *cursor = L'\0';
          GlobalUnlock(memory);
          if (SetClipboardData(CF_HDROP, memory) != nullptr) {
            applied = true;
          } else {
            GlobalFree(memory);
          }
        } else {
          GlobalFree(memory);
        }
      }
      CloseClipboard();
      return applied;
    }
    return applied;
  }

 private:
  std::mutex watch_mutex_;
  text_watcher watch_callback_;
  files_watcher files_callback_;
  std::string last_text_;
  std::string last_files_;
  DWORD sequence_ = 0;
  std::atomic<bool> running_{false};
  std::thread watcher_;
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
