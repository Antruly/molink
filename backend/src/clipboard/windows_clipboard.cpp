#include "clipboard/clipboard.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "clipboard/clipboard_files.h"
#include "clipboard/windows_data_object.h"
#include "clipboard/windows_virtual_clipboard.h"
#include "clipboard_lazy.h"

namespace molink {
namespace {

constexpr UINT kLazyStopMessage = WM_APP + 0x51;
constexpr wchar_t kLazyWindowClass[] = L"moLinkLazyClipboard";

/// A describe request must not stall the caller behind a stream read for long;
/// the caller falls back to CF_HDROP when the data-object thread is busy.
constexpr uint64_t kObjectDescribeTimeoutMs = 2000;
/// One clipboard-pull range may spend at most this long inside IStream::Read.
/// The viewer's own 0x18 timeout is longer, so the host reports first.
constexpr uint64_t kObjectReadTimeoutMs = 15000;
/// One file's whole pull may spend at most this long inside the data object.
constexpr uint64_t kObjectReadTotalMs = 60000;

bool clipboard_log_enabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("MOLINK_CLIPBOARD_LOG");
    return value != nullptr && std::string(value) == "1";
  }();
  return enabled;
}

void clipboard_log(const char* format, ...) {
  if (!clipboard_log_enabled()) return;
  std::fprintf(stderr, "molink: clipboard ");
  va_list args;
  va_start(args, format);
  std::vfprintf(stderr, format, args);
  va_end(args);
  std::fprintf(stderr, "\n");
}

uint64_t steady_ms() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

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

bool open_clipboard_retry(HWND owner, int attempts = 10) {
  for (int i = 0; i < attempts; ++i) {
    if (OpenClipboard(owner)) return true;
    Sleep(10);
  }
  return false;
}

LRESULT CALLBACK clipboard_owner_proc(HWND window, UINT message, WPARAM wparam,
                                      LPARAM lparam) {
  // The window exists only so GetClipboardOwner() can name this process. The
  // delayed-render messages belong to the lazy thread; this one is used for
  // the eager text/file paths, which use real handles.
  (void)wparam;
  (void)lparam;
  if (message == WM_RENDERALLFORMATS || message == WM_DESTROYCLIPBOARD) {
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

HWND create_clipboard_owner_window() {
  static const wchar_t kClass[] = L"moLinkClipboardOwner";
  WNDCLASSEXW window_class = {};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = clipboard_owner_proc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.lpszClassName = kClass;
  RegisterClassExW(&window_class);  // already registered is fine
  return CreateWindowExW(0, kClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                         GetModuleHandleW(nullptr), nullptr);
}

/// Who owns the clipboard, from this process's point of view.
///
/// `GetClipboardOwner` names the window passed to OpenClipboard; OLE's
/// delayed rendering uses a window owned by the OLE apartment in this same
/// process. When the owner is null (a task-associated open, or a clipboard
/// manager that has already released it) the window currently holding the
/// clipboard is the next best witness. Anything else -- mstsc, TeamViewer,
/// Sunlogin, RustDesk, a plain application -- is `external` and must be
/// accepted as a normal local copy.
clipboard_owner classify_clipboard_owner(HWND self_window) {
  const HWND owner = GetClipboardOwner();
  const HWND probe = owner != nullptr ? owner : GetOpenClipboardWindow();
  if (probe == nullptr) return clipboard_owner::unknown;
  if (self_window != nullptr && probe == self_window) {
    return clipboard_owner::self;
  }
  DWORD process_id = 0;
  GetWindowThreadProcessId(probe, &process_id);
  if (process_id != 0 && process_id == GetCurrentProcessId()) {
    return clipboard_owner::self;
  }
  return clipboard_owner::external;
}

/// Cheap "might this clipboard hold files?" probe. Reading a text copy must
/// not pay for the OLE describe (up to two seconds), but a copy of files that
/// also exposes text has to be announced as both.
bool clipboard_may_hold_files() {
  if (!open_clipboard_retry(nullptr, 3)) return false;
  const bool files = IsClipboardFormatAvailable(CF_HDROP) ||
                     IsClipboardFormatAvailable(file_descriptor_format());
  CloseClipboard();
  return files;
}

/// mstsc's main window class. Its presence means an inner Windows session is
/// connected; the viewer is told so it can show an "inner RDP" badge.
BOOL CALLBACK find_rdp_window(HWND window, LPARAM found) {
  wchar_t name[64] = {};
  if (GetClassNameW(window, name,
                    static_cast<int>(sizeof(name) / sizeof(name[0]))) == 0) {
    return TRUE;
  }
  if (wcscmp(name, L"TscShellContainerClass") == 0) {
    *reinterpret_cast<bool*>(found) = true;
    return FALSE;
  }
  return TRUE;
}

/// Builds the CF_HDROP block for a list of real paths.
HGLOBAL make_hdrop(const std::vector<std::string>& paths) {
  std::vector<std::wstring> wide;
  wide.reserve(paths.size());
  for (const std::string& path : paths) {
    std::wstring converted = to_wide(path);
    if (!converted.empty()) wide.push_back(std::move(converted));
  }
  if (wide.empty()) return nullptr;
  std::size_t characters = 1;  // final list terminator
  for (const std::wstring& path : wide) characters += path.size() + 1;
  const std::size_t bytes = sizeof(DROPFILES) + characters * sizeof(wchar_t);
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (memory == nullptr) return nullptr;
  auto* drop = static_cast<DROPFILES*>(GlobalLock(memory));
  if (drop == nullptr) {
    GlobalFree(memory);
    return nullptr;
  }
  ZeroMemory(drop, sizeof(DROPFILES));
  drop->pFiles = sizeof(DROPFILES);
  drop->fWide = TRUE;
  auto* cursor = reinterpret_cast<wchar_t*>(
      reinterpret_cast<BYTE*>(drop) + sizeof(DROPFILES));
  for (const std::wstring& path : wide) {
    std::memcpy(cursor, path.c_str(), path.size() * sizeof(wchar_t));
    cursor += path.size();
    *cursor++ = L'\0';
  }
  *cursor = L'\0';
  GlobalUnlock(memory);
  return memory;
}

/// Attributes and size of one path named by CF_HDROP. A directory is reported
/// with `FILE_ATTRIBUTE_DIRECTORY`; its `size` is meaningless.
struct clipboard_path_attributes {
  DWORD attributes = 0;
  uint64_t size = 0;
};

/// Looks up one local path without reading any of its bytes.
///
/// `FindFirstFileW` answers the size from the directory entry, which is what
/// makes a 200 MB Explorer copy cost nothing on the host. The handle fallback
/// covers names the wildcard API refuses.
bool query_clipboard_path(const std::wstring& path,
                          clipboard_path_attributes& out) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) return false;
  out.attributes = attributes;
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    out.size = 0;
    return true;
  }
  WIN32_FIND_DATAW data = {};
  HANDLE find = FindFirstFileW(path.c_str(), &data);
  if (find != INVALID_HANDLE_VALUE) {
    FindClose(find);
    out.attributes = data.dwFileAttributes;
    out.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) |
               static_cast<uint64_t>(data.nFileSizeLow);
    return true;
  }
  HANDLE file = CreateFileW(
      path.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  BY_HANDLE_FILE_INFORMATION info = {};
  const bool ok = GetFileInformationByHandle(file, &info) != 0;
  CloseHandle(file);
  if (!ok) return false;
  out.attributes = info.dwFileAttributes;
  out.size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) |
             static_cast<uint64_t>(info.nFileSizeLow);
  return true;
}

/// Result of walking a CF_HDROP list. `entries` names regular files only;
/// directories are expanded in place, so a folder copied in Explorer reaches
/// the viewer as its contents instead of vanishing.
struct hdrop_scan_result {
  std::vector<clipboard_path_entry> entries;
  std::size_t directories = 0;
  std::size_t unreadable = 0;
  std::size_t empty_directories = 0;
  bool truncated = false;
};

void scan_directory_tree(const std::filesystem::path& directory,
                         unsigned depth, hdrop_scan_result& result);

/// Adds one regular file, stopping at kClipboardPathScanMax.
void add_scanned_file(const std::filesystem::path& path,
                      hdrop_scan_result& result) {
  if (result.entries.size() >= kClipboardPathScanMax) {
    result.truncated = true;
    return;
  }
  clipboard_path_attributes info;
  if (!query_clipboard_path(path.wstring(), info)) {
    ++result.unreadable;
    return;
  }
  clipboard_path_entry entry;
  entry.path = to_utf8(path.wstring().c_str());
  entry.name = to_utf8(path.filename().wstring().c_str());
  entry.size = info.size;
  if (entry.path.empty() || entry.name.empty()) {
    ++result.unreadable;
    return;
  }
  result.entries.push_back(std::move(entry));
}

void scan_directory_tree(const std::filesystem::path& directory,
                         unsigned depth, hdrop_scan_result& result) {
  if (result.entries.size() >= kClipboardPathScanMax) {
    result.truncated = true;
    return;
  }
  // A junction loop must not turn one clipboard copy into an endless walk.
  constexpr unsigned kMaxDepth = 8;
  if (depth > kMaxDepth) {
    result.truncated = true;
    clipboard_log("CF_HDROP directory depth limit reached");
    return;
  }
  std::error_code ec;
  std::filesystem::directory_iterator it(directory, ec);
  if (ec) {
    ++result.unreadable;
    clipboard_log("CF_HDROP cannot read directory %s",
                  to_utf8(directory.wstring().c_str()).c_str());
    return;
  }
  for (const std::filesystem::directory_entry& item : it) {
    if (result.entries.size() >= kClipboardPathScanMax) {
      result.truncated = true;
      return;
    }
    std::error_code entry_ec;
    const std::filesystem::file_status status =
        item.symlink_status(entry_ec);
    if (entry_ec) {
      ++result.unreadable;
      continue;
    }
    if (std::filesystem::is_symlink(status)) continue;
    if (std::filesystem::is_directory(status)) {
      ++result.directories;
      scan_directory_tree(item.path(), depth + 1, result);
    } else if (std::filesystem::is_regular_file(status)) {
      add_scanned_file(item.path(), result);
    }
  }
}

/// Copies the paths out of a CF_HDROP block. The clipboard is released before
/// the caller stats or walks anything: holding it open while enumerating a
/// directory would stall the application that owns it.
bool read_hdrop_paths(IDataObject* object, std::vector<std::wstring>& paths) {
  STGMEDIUM medium = {};
  bool have_medium = false;
  HDROP drop = nullptr;
  if (object != nullptr) {
    FORMATETC format = {};
    format.cfFormat = CF_HDROP;
    format.dwAspect = DVASPECT_CONTENT;
    format.lindex = -1;
    format.tymed = TYMED_HGLOBAL;
    if (SUCCEEDED(object->GetData(&format, &medium))) {
      if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal != nullptr) {
        drop = static_cast<HDROP>(medium.hGlobal);
        have_medium = true;
      } else {
        ReleaseStgMedium(&medium);
      }
    }
  }
  bool opened = false;
  if (drop == nullptr) {
    opened = open_clipboard_retry(nullptr);
    if (!opened) {
      clipboard_log("CF_HDROP: clipboard is busy");
      return false;
    }
    if (!IsClipboardFormatAvailable(CF_HDROP)) {
      CloseClipboard();
      return false;
    }
    HANDLE data = GetClipboardData(CF_HDROP);
    if (data == nullptr) {
      clipboard_log("CF_HDROP: GetClipboardData failed (%lu)",
                    static_cast<unsigned long>(GetLastError()));
      CloseClipboard();
      return false;
    }
    drop = static_cast<HDROP>(data);
  }
  const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
  paths.reserve(count);
  for (UINT i = 0; i < count; ++i) {
    const UINT length = DragQueryFileW(drop, i, nullptr, 0);
    if (length == 0) continue;
    std::wstring path(length + 1, L'\0');
    if (DragQueryFileW(drop, i, path.data(), length + 1) == 0) continue;
    path.resize(length);
    paths.push_back(std::move(path));
  }
  if (have_medium) ReleaseStgMedium(&medium);
  if (opened) CloseClipboard();
  return true;
}

/// Enumerates a plain file copy (Explorer / `SetFileDropList`), expanding
/// directories recursively up to kClipboardPathScanMax. Returns false when
/// the clipboard does not offer CF_HDROP at all.
bool scan_hdrop(IDataObject* object, hdrop_scan_result& result) {
  std::vector<std::wstring> paths;
  if (!read_hdrop_paths(object, paths)) return false;
  for (const std::wstring& path_text : paths) {
    if (result.entries.size() >= kClipboardPathScanMax) {
      result.truncated = true;
      break;
    }
    const std::filesystem::path path(path_text);
    clipboard_path_attributes info;
    if (!query_clipboard_path(path_text, info)) {
      ++result.unreadable;
      clipboard_log("CF_HDROP path unavailable: %s",
                    to_utf8(path_text.c_str()).c_str());
      continue;
    }
    if ((info.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      ++result.directories;
      const std::size_t before = result.entries.size();
      scan_directory_tree(path, 1, result);
      if (result.entries.size() == before) ++result.empty_directories;
      continue;
    }
    clipboard_path_entry entry;
    entry.path = to_utf8(path_text.c_str());
    const std::wstring name = path.filename().wstring();
    entry.name = to_utf8(name.c_str());
    entry.size = info.size;
    if (entry.path.empty() || entry.name.empty()) {
      ++result.unreadable;
      continue;
    }
    result.entries.push_back(std::move(entry));
  }
  return true;
}

/// Builds the FILEGROUPDESCRIPTORW block Explorer/OLE applications use to
/// enumerate virtual files. It is available immediately: only the contents are
/// delayed.
HGLOBAL make_file_descriptor(const std::vector<lazy_clipboard_file>& files) {
  const std::size_t count = std::max<std::size_t>(1, files.size());
  const std::size_t bytes =
      sizeof(UINT) + count * sizeof(FILEDESCRIPTORW);
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
  if (memory == nullptr) return nullptr;
  void* locked = GlobalLock(memory);
  if (locked == nullptr) {
    GlobalFree(memory);
    return nullptr;
  }
  auto* descriptor = static_cast<FILEGROUPDESCRIPTORW*>(locked);
  descriptor->cItems = static_cast<UINT>(files.size());
  for (std::size_t i = 0; i < files.size(); ++i) {
    FILEDESCRIPTORW& entry = descriptor->fgd[i];
    entry.dwFlags = FD_ATTRIBUTES | FD_FILESIZE;
    entry.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    entry.nFileSizeHigh =
        static_cast<DWORD>((files[i].size >> 32) & 0xffffffffull);
    entry.nFileSizeLow = static_cast<DWORD>(files[i].size & 0xffffffffull);
    const std::wstring name = to_wide(files[i].name);
    const std::size_t copied =
        std::min<std::size_t>(name.size(), MAX_PATH - 1);
    std::memcpy(entry.cFileName, name.c_str(), copied * sizeof(wchar_t));
    entry.cFileName[copied] = L'\0';
  }
  GlobalUnlock(memory);
  return memory;
}

/// An IStream over a file that is already complete on disk. FileContents
/// consumers receive one of these per file index.
class lazy_file_stream final : public IStream {
 public:
  static IStream* open(const std::string& path, std::string& error) {
    const std::wstring wide = to_wide(path);
    HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      error = "could not open the rendered file";
      return nullptr;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) {
      CloseHandle(file);
      error = "could not size the rendered file";
      return nullptr;
    }
    return new lazy_file_stream(file, static_cast<uint64_t>(size.QuadPart));
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void** object) override {
    if (object == nullptr) return E_INVALIDARG;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_IStream ||
        riid == IID_ISequentialStream) {
      *object = static_cast<IStream*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return static_cast<ULONG>(++references_);
  }

  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG value = static_cast<ULONG>(--references_);
    if (value == 0) delete this;
    return value;
  }

  HRESULT STDMETHODCALLTYPE Read(void* buffer, ULONG bytes,
                                 ULONG* read) override {
    if (read != nullptr) *read = 0;
    if (file_ == INVALID_HANDLE_VALUE) return STG_E_ACCESSDENIED;
    if (buffer == nullptr || bytes == 0) return S_OK;
    LARGE_INTEGER offset{};
    offset.QuadPart = static_cast<LONGLONG>(position_);
    if (!SetFilePointerEx(file_, offset, nullptr, FILE_BEGIN)) {
      return STG_E_READFAULT;
    }
    DWORD done = 0;
    if (!::ReadFile(file_, buffer, bytes, &done, nullptr)) {
      return STG_E_READFAULT;
    }
    position_ += done;
    if (read != nullptr) *read = done;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override {
    return STG_E_ACCESSDENIED;
  }

  HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin,
                                 ULARGE_INTEGER* new_position) override {
    LONGLONG base = 0;
    if (origin == STREAM_SEEK_SET) {
      base = 0;
    } else if (origin == STREAM_SEEK_CUR) {
      base = static_cast<LONGLONG>(position_);
    } else if (origin == STREAM_SEEK_END) {
      base = static_cast<LONGLONG>(size_);
    } else {
      return STG_E_INVALIDFUNCTION;
    }
    const LONGLONG target = base + move.QuadPart;
    if (target < 0) return STG_E_INVALIDFUNCTION;
    position_ = static_cast<uint64_t>(target);
    if (new_position != nullptr) {
      new_position->QuadPart = static_cast<ULONGLONG>(position_);
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override {
    return STG_E_ACCESSDENIED;
  }

  HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*,
                                   ULARGE_INTEGER*) override {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }

  HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }

  HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER,
                                       DWORD) override {
    return STG_E_INVALIDFUNCTION;
  }

  HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER,
                                         DWORD) override {
    return STG_E_INVALIDFUNCTION;
  }

  HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD) override {
    if (stat == nullptr) return E_INVALIDARG;
    ZeroMemory(stat, sizeof(STATSTG));
    stat->type = STGTY_STREAM;
    stat->cbSize.QuadPart = size_;
    stat->grfMode = STGM_READ;
    stat->grfLocksSupported = 0;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }

 private:
  lazy_file_stream(HANDLE file, uint64_t size)
      : file_(file), size_(size) {}
  ~lazy_file_stream() {
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
  }

  std::atomic<ULONG> references_{1};
  HANDLE file_ = INVALID_HANDLE_VALUE;
  uint64_t size_ = 0;
  uint64_t position_ = 0;
};

/// IStream over a clipboard file whose bytes stay on the viewer's machine.
///
/// This is the RDP-style path: the host never stages the file. Each Read()
/// posts a `fetch` for the next 64 KiB block and waits for the matching
/// `clipboard_range` binary reply. Explorer calls Read() from its paste
/// thread, so blocking here is exactly what drives the copy dialog's progress
/// bar; the timeouts in read_lazy_blob guarantee it cannot hang forever.
///
/// The stream survives the data object: it owns a shared_ptr to the source,
/// and a Clone shares that source while keeping its own seek position.
class remote_file_stream final : public IStream {
 public:
  static IStream* open(std::shared_ptr<lazy_blob_source> source,
                       std::size_t index) {
    if (!source || index >= source->size()) return nullptr;
    return new remote_file_stream(std::move(source), index);
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void** object) override {
    if (object == nullptr) return E_INVALIDARG;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_IStream ||
        riid == IID_ISequentialStream) {
      *object = static_cast<IStream*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return static_cast<ULONG>(++references_);
  }

  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG value = static_cast<ULONG>(--references_);
    if (value == 0) delete this;
    return value;
  }

  HRESULT STDMETHODCALLTYPE Read(void* buffer, ULONG bytes,
                                 ULONG* read) override {
    if (read != nullptr) *read = 0;
    if (bytes == 0) return S_OK;
    if (buffer == nullptr) return STG_E_INVALIDPOINTER;
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t done = 0;
    std::string error;
    if (!read_lazy_blob(*source_, index_, position_,
                        static_cast<uint8_t*>(buffer),
                        static_cast<uint32_t>(bytes), done, error)) {
      return STG_E_READFAULT;
    }
    if (read != nullptr) *read = done;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override {
    return STG_E_ACCESSDENIED;
  }

  HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin,
                                 ULARGE_INTEGER* new_position) override {
    lazy_seek_origin kind = lazy_seek_origin::begin;
    if (origin == STREAM_SEEK_SET) {
      kind = lazy_seek_origin::begin;
    } else if (origin == STREAM_SEEK_CUR) {
      kind = lazy_seek_origin::current;
    } else if (origin == STREAM_SEEK_END) {
      kind = lazy_seek_origin::end;
    } else {
      return STG_E_INVALIDFUNCTION;
    }
    uint64_t target = 0;
    if (!lazy_blob_seek(size_, position_, move.QuadPart, kind, target)) {
      return STG_E_INVALIDFUNCTION;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    position_ = target;
    if (new_position != nullptr) {
      new_position->QuadPart = static_cast<ULONGLONG>(target);
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override {
    return STG_E_ACCESSDENIED;
  }

  HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*,
                                   ULARGE_INTEGER*) override {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }

  HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }

  HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER,
                                       DWORD) override {
    return STG_E_INVALIDFUNCTION;
  }

  HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER,
                                         DWORD) override {
    return STG_E_INVALIDFUNCTION;
  }

  HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD) override {
    if (stat == nullptr) return E_INVALIDARG;
    ZeroMemory(stat, sizeof(STATSTG));
    stat->type = STGTY_STREAM;
    stat->cbSize.QuadPart = size_;
    stat->grfMode = STGM_READ;
    stat->grfLocksSupported = 0;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Clone(IStream** output) override {
    if (output == nullptr) return E_INVALIDARG;
    *output = nullptr;
    auto* clone = new remote_file_stream(source_, index_);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      clone->position_ = position_;
    }
    *output = clone;
    return S_OK;
  }

 private:
  remote_file_stream(std::shared_ptr<lazy_blob_source> source,
                     std::size_t index)
      : source_(std::move(source)),
        index_(index),
        size_(source_->files()[index_].size) {}

  std::atomic<ULONG> references_{1};
  std::shared_ptr<lazy_blob_source> source_;
  std::size_t index_ = 0;
  uint64_t size_ = 0;
  std::mutex mutex_;
  uint64_t position_ = 0;
};

/// Reads the *host's* clipboard through its OLE data object.
///
/// mstsc (and msrdc) expose a file copied inside an inner RDP session as a
/// virtual file: FileGroupDescriptorW names the entries and FileContents
/// answers byte ranges. Reading that stream is the whole point -- the file is
/// never materialised on this machine. All of it lives on one dedicated STA
/// thread because `IDataObject::GetData` and `IStream::Read` may block on the
/// RDP channel; the libuv loop and the clipboard watcher never call into OLE.
///
/// The object is cached per clipboard sequence number. A new sequence means
/// mstsc replaced its offer, so every cached stream is released before the new
/// generation is announced. Each entry carries a token that names its
/// generation; a read with a stale token is answered with `gone` instead of
/// bytes from the wrong file.
class clipboard_object_reader final {
 public:
  clipboard_object_reader() : thread_([this] { run(); }) {}

  ~clipboard_object_reader() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }

  clipboard_object_reader(const clipboard_object_reader&) = delete;
  clipboard_object_reader& operator=(const clipboard_object_reader&) = delete;

  /// Enumerates the clipboard. Returns false when there is neither an OLE
  /// virtual-file offer nor a CF_HDROP list (or the worker did not answer in
  /// time). `source` reports `ole` for FileContents streams and `hdrop` for a
  /// plain local-path copy.
  bool describe(std::vector<clipboard_file>& files, uint64_t timeout_ms,
                std::string* source = nullptr) {
    if (wedged_->load()) return false;
    auto request = std::make_shared<job>();
    request->type = job::kind::describe;
    request->sequence = GetClipboardSequenceNumber();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_ || queue_.size() >= 8) return false;
      queue_.push_back(request);
    }
    cv_.notify_one();

    std::unique_lock<std::mutex> lock(request->mutex);
    if (!request->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                              [&request] { return request->done; })) {
      clipboard_log("data-object describe timed out");
      return false;
    }
    if (!request->ok || request->files.empty()) return false;
    files = std::move(request->files);
    if (source != nullptr) *source = request->source;
    return true;
  }

  /// Queues a range read. The callback runs on the worker (or on the timeout
  /// thread when the RDP channel does not answer).
  void read(const clipboard_file& file, uint64_t offset, uint64_t length,
            clipboard_service::file_range_callback callback,
            uint32_t per_read_timeout_ms, uint32_t total_timeout_ms) {
    if (!callback) return;
    uint32_t generation = 0;
    uint32_t index = 0;
    if (!clipboard_stream_token_split(file.stream_id, generation, index)) {
      callback(2, offset, {});
      return;
    }
    if (wedged_->load()) {
      callback(2, offset, {});
      return;
    }
    // A whole upstream file may take longer than one range; after the total
    // budget the clipboard has effectively moved on.
    {
      std::lock_guard<std::mutex> lock(budget_mutex_);
      const uint64_t now = steady_ms();
      const auto key = std::make_pair(generation, index);
      const auto it = read_started_.find(key);
      if (it == read_started_.end()) {
        read_started_[key] = now;
      } else if (now >= it->second && now - it->second > total_timeout_ms) {
        read_started_.erase(it);
        clipboard_log("data-object read budget exhausted generation=%u index=%u",
                      generation, index);
        callback(2, offset, {});
        return;
      }
    }

    auto state = std::make_shared<read_state>();
    auto request = std::make_shared<job>();
    request->type = job::kind::read;
    request->file = file;
    request->offset = offset;
    request->length = length;
    request->callback = std::move(callback);
    request->state = state;
    bool posted = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!stopping_) {
        queue_.push_back(request);
        posted = true;
      }
    }
    if (!posted) {
      if (request->callback) request->callback(2, offset, {});
      return;
    }
    cv_.notify_one();

    // IStream::Read cannot be interrupted portably. Answer the viewer on time
    // and let the worker discard the late result; `wedged_` makes every
    // request until it recovers fail fast instead of queueing behind it.
    try {
      std::thread([state, request, offset, per_read_timeout_ms,
                   wedged = wedged_] {
        std::unique_lock<std::mutex> lock(state->mutex);
        if (state->cv.wait_for(lock,
                               std::chrono::milliseconds(per_read_timeout_ms),
                               [&state] { return state->done; })) {
          return;
        }
        state->done = true;
        lock.unlock();
        request->timed_out.store(true);
        wedged->store(true);
        if (request->callback) {
          // The callback posts to the session loop. If that throws, the core
          // must keep running; the viewer's own pull timeout covers it.
          try {
            request->callback(2, offset, {});
          } catch (...) {
            clipboard_log("data-object timeout callback threw");
          }
        }
      }).detach();
    } catch (const std::exception& error) {
      // Out of threads: the queued read still runs on the worker, so the
      // callback will be delivered; only the deadline is lost.
      clipboard_log("data-object timeout thread failed: %s", error.what());
    }
  }

 private:
  struct read_state {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
  };

  struct job {
    enum class kind { describe, read } type = kind::describe;
    // describe
    DWORD sequence = 0;
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool ok = false;
    std::string source;
    std::vector<clipboard_file> files;
    // read
    clipboard_file file;
    uint64_t offset = 0;
    uint64_t length = 0;
    clipboard_service::file_range_callback callback;
    std::shared_ptr<read_state> state;
    std::atomic<bool> timed_out{false};
  };

  /// Answers a job that could not be completed. Used only by the exception
  /// guard below, so a malformed OLE object cannot take down the core.
  void fail_job(job& request, uint32_t status) {
    if (request.type == job::kind::describe) {
      {
        std::lock_guard<std::mutex> lock(request.mutex);
        request.ok = false;
        request.files.clear();
        request.done = true;
      }
      request.cv.notify_all();
      return;
    }
    bool deliver = false;
    {
      std::lock_guard<std::mutex> lock(request.state->mutex);
      if (!request.state->done) {
        request.state->done = true;
        deliver = true;
      }
    }
    if (deliver && request.callback) {
      try {
        request.callback(status, request.offset, {});
      } catch (...) {
        clipboard_log("data-object failure callback threw");
      }
    }
  }

  void run() {
    const HRESULT result = OleInitialize(nullptr);
    ole_ = SUCCEEDED(result);
    clipboard_log("data-object worker started (ole=%s)",
                  ole_ ? "ok" : "unavailable");
    for (;;) {
      std::shared_ptr<job> next;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (stopping_ && queue_.empty()) break;
        next = queue_.front();
        queue_.pop_front();
      }
      // IDataObject/COM may throw std::bad_alloc or a vendor exception on a
      // broken RDP channel. An exception escaping this thread calls
      // std::terminate and takes the whole core (and every session) with it,
      // which is exactly what a clipboard-pull used to do.
      try {
        if (next->type == job::kind::describe) {
          handle_describe(*next);
        } else {
          handle_read(*next);
        }
      } catch (const std::exception& error) {
        clipboard_log("data-object worker exception: %s", error.what());
        fail_job(*next, 2);
      } catch (...) {
        clipboard_log("data-object worker unknown exception");
        fail_job(*next, 2);
      }
    }
    release_cache();
    if (ole_) OleUninitialize();
  }

  void handle_describe(job& request) {
    std::vector<clipboard_file> files;
    std::string source;
    bool ok = false;
    if (ole_) {
      const DWORD sequence = GetClipboardSequenceNumber();
      // A describe posted before the clipboard moved on must not replace the
      // newer generation a later caller already used.
      if (request.sequence == 0 || request.sequence == sequence) {
        ok = refresh_cache(sequence, files, source);
      }
    }
    {
      std::lock_guard<std::mutex> lock(request.mutex);
      request.ok = ok;
      request.source = std::move(source);
      request.files = std::move(files);
      request.done = true;
    }
    request.cv.notify_all();
    wedged_->store(false);
  }

  void handle_read(job& request) {
    const uint64_t started = steady_ms();
    uint32_t status = 2;
    std::vector<uint8_t> data;
    uint32_t generation = 0;
    uint32_t index = 0;
    if (!clipboard_stream_token_split(request.file.stream_id, generation,
                                      index)) {
      status = 2;
    } else if (object_ == nullptr || !cache_valid_ ||
               generation != generation_ ||
               GetClipboardSequenceNumber() != cached_sequence_) {
      // The clipboard moved on (mstsc replaced its offer). Releasing here is
      // what makes the generation model observable to the viewer as status 2.
      release_cache();
      status = 2;
    } else if (index >= public_files_.size()) {
      status = 2;
    } else {
      const clipboard_file& advertised = public_files_[index];
      if (advertised.name != request.file.name ||
          advertised.size != request.file.size ||
          request.offset > advertised.size) {
        status = 2;
      } else {
        IStream* stream = stream_for(descriptions_[index].lindex);
        status = stream == nullptr
                     ? 1
                     : read_data_object_stream(stream, request.offset,
                                               request.length, data);
      }
    }

    bool deliver = false;
    {
      std::lock_guard<std::mutex> lock(request.state->mutex);
      if (!request.state->done) {
        request.state->done = true;
        deliver = true;
      }
    }
    if (deliver) {
      clipboard_log(
          "data-object read index=%u offset=%llu length=%llu status=%u "
          "elapsed=%llums",
          index, static_cast<unsigned long long>(request.offset),
          static_cast<unsigned long long>(request.length), status,
          static_cast<unsigned long long>(steady_ms() - started));
      if (request.callback) {
        request.callback(status, request.offset, std::move(data));
      }
    } else {
      clipboard_log("data-object read index=%u finished after its timeout; "
                    "reply discarded",
                    index);
    }
    wedged_->store(false);
  }

  IStream* stream_for(long lindex) {
    const auto existing = streams_.find(lindex);
    if (existing != streams_.end()) return existing->second->stream;
    auto stream = std::make_unique<data_object_stream>();
    if (!open_data_object_stream(object_, lindex, *stream)) {
      clipboard_log("data-object stream lindex=%ld unavailable", lindex);
      return nullptr;
    }
    IStream* raw = stream->stream;
    streams_.emplace(lindex, std::move(stream));
    return raw;
  }

  bool refresh_cache(DWORD sequence, std::vector<clipboard_file>& out,
                     std::string& source) {
    if (cache_valid_ && cached_sequence_ == sequence) {
      out = public_files_;
      source = source_;
      return !out.empty();
    }
    release_cache();
    cache_valid_ = true;
    cached_sequence_ = sequence;

    IDataObject* object = nullptr;
    const HRESULT result = OleGetClipboard(&object);
    if (FAILED(result) || object == nullptr) {
      // A plain SetClipboardData(CF_HDROP) owner still has a readable
      // clipboard even when OLE cannot wrap it, so do not give up here.
      clipboard_log("OleGetClipboard failed (0x%08lx); trying CF_HDROP",
                    static_cast<unsigned long>(result));
    }

    std::vector<data_object_file> descriptions;
    if (object != nullptr && describe_data_object(object, descriptions)) {
      // Inner RDP (mstsc/msrdc): the bytes stay behind FileContents streams
      // and are read range by range through the OLE data object.
      generation_ += 1;
      if (generation_ == 0) generation_ = 1;
      object_ = object;
      descriptions_ = std::move(descriptions);
      public_files_.clear();
      public_files_.reserve(descriptions_.size());
      for (std::size_t i = 0; i < descriptions_.size(); ++i) {
        clipboard_file file;
        file.name = descriptions_[i].name;
        file.size = descriptions_[i].size;
        file.stream_id = clipboard_stream_token(
            generation_, static_cast<uint32_t>(i));
        public_files_.push_back(std::move(file));
      }
      source_ = "ole";
      source = source_;
      clipboard_log("data-object files=%zu generation=%u",
                    public_files_.size(), generation_);
      out = public_files_;
      return !out.empty();
    }

    // Explorer's ordinary Ctrl+C only offers CF_HDROP. Enumerate it here, on
    // the same worker, so `describe()` answers for both kinds of copy. The
    // paths are real local files; no FileContents stream is involved.
    hdrop_scan_result scan;
    const bool has_hdrop = scan_hdrop(object, scan);
    if (object != nullptr) object->Release();
    if (!has_hdrop) {
      clipboard_log(
          "clipboard has neither FileContents streams nor CF_HDROP");
      return false;
    }
    clipboard_path_build built = build_clipboard_files(scan.entries);
    if (built.invalid > 0) {
      clipboard_log("CF_HDROP skipped %zu unusable path(s)", built.invalid);
    }
    if (scan.unreadable > 0 || scan.empty_directories > 0 ||
        scan.truncated) {
      clipboard_log(
          "CF_HDROP directories=%zu empty=%zu unreadable=%zu truncated=%d",
          scan.directories, scan.empty_directories, scan.unreadable,
          scan.truncated ? 1 : 0);
    }
    if (built.files.empty()) return false;

    generation_ += 1;
    if (generation_ == 0) generation_ = 1;
    // No OLE object and no per-file tokens: read_clipboard_pull() serves this
    // list from the paths, one requested range at a time.
    object_ = nullptr;
    descriptions_.clear();
    public_files_ = std::move(built.files);
    source_ = "hdrop";
    source = source_;
    clipboard_log(
        "CF_HDROP files=%zu directories=%zu truncated=%d generation=%u",
        public_files_.size(), scan.directories, scan.truncated ? 1 : 0,
        generation_);
    out = public_files_;
    return !out.empty();
  }

  void release_cache() {
    if (object_ != nullptr) {
      object_->Release();
      object_ = nullptr;
    }
    streams_.clear();
    descriptions_.clear();
    public_files_.clear();
    source_.clear();
    cache_valid_ = false;
    cached_sequence_ = 0;
    {
      std::lock_guard<std::mutex> lock(budget_mutex_);
      read_started_.clear();
    }
  }

  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::shared_ptr<job>> queue_;
  bool stopping_ = false;

  /// Set when a read outlived kObjectReadTimeoutMs. The worker still owns the
  /// blocked IStream; every new request fails fast until it returns.
  std::shared_ptr<std::atomic<bool>> wedged_ =
      std::make_shared<std::atomic<bool>>(false);

  /// Worker-thread-only cache.
  bool ole_ = false;
  bool cache_valid_ = false;
  DWORD cached_sequence_ = 0;
  uint32_t generation_ = 0;
  IDataObject* object_ = nullptr;
  std::vector<data_object_file> descriptions_;
  std::vector<clipboard_file> public_files_;
  /// `ole` (FileContents streams) or `hdrop` (real local paths). Carried to
  /// the caller so the announce log names the source.
  std::string source_;
  std::map<long, std::unique_ptr<data_object_stream>> streams_;

  /// Shared with the caller threads that enforce the per-file total budget.
  std::mutex budget_mutex_;
  std::map<std::pair<uint32_t, uint32_t>, uint64_t> read_started_;
};

/// OLE data object for the lazy offer. Explorer and RDP-style consumers query
/// CF_HDROP, FileGroupDescriptorW and FileContents from it. A FileContents
/// request names one file through FORMATETC.lindex; its IStream is created
/// only after the bytes for that file have arrived (eager mode) or streams
/// them range by range (streaming mode).
///
/// In streaming mode CF_HDROP is deliberately absent: advertising it would
/// make Explorer copy every byte into a staged path first, which is the very
/// detour this mode exists to avoid.
class lazy_data_object final : public IDataObject {
 public:
  explicit lazy_data_object(std::shared_ptr<const lazy_offer> offer)
      : offer_(std::move(offer)),
        streaming_(offer_ && offer_->source && offer_->source->streaming()) {}

  bool streaming() const { return streaming_; }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void** object) override {
    if (object == nullptr) return E_INVALIDARG;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_IDataObject) {
      *object = static_cast<IDataObject*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return static_cast<ULONG>(++references_);
  }

  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG value = static_cast<ULONG>(--references_);
    if (value == 0) delete this;
    return value;
  }

  HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format,
                                    STGMEDIUM* medium) override {
    if (format == nullptr || medium == nullptr) return E_INVALIDARG;
    if (format->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
    const UINT cf = format->cfFormat;
    if (cf == CF_HDROP) {
      // A virtual file paste must go through FileContents, not through a path
      // the host does not have. Offering CF_HDROP would send Explorer to a
      // staged copy instead.
      if (streaming_) return DV_E_FORMATETC;
      if ((format->tymed & TYMED_HGLOBAL) == 0) return DV_E_TYMED;
      return get_hdrop(medium);
    }
    if (cf == file_descriptor_format()) {
      if ((format->tymed & TYMED_HGLOBAL) == 0) return DV_E_TYMED;
      return get_descriptor(medium);
    }
    if (cf == file_contents_format()) {
      long index = format->lindex;
      if (index < 0 && offer_->files.size() == 1) index = 0;
      if (index < 0 ||
          static_cast<std::size_t>(index) >= offer_->files.size()) {
        return DV_E_LINDEX;
      }
      if ((format->tymed & TYMED_ISTREAM) != 0) {
        return get_stream(static_cast<std::size_t>(index), medium);
      }
      if ((format->tymed & TYMED_HGLOBAL) != 0) {
        return get_contents(static_cast<std::size_t>(index), medium);
      }
      return DV_E_TYMED;
    }
    return DV_E_FORMATETC;
  }

  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
    if (format == nullptr) return E_INVALIDARG;
    if (format->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
    const UINT cf = format->cfFormat;
    if (cf == CF_HDROP) {
      if (streaming_) return DV_E_FORMATETC;
      return (format->tymed & TYMED_HGLOBAL) != 0 ? S_OK : DV_E_TYMED;
    }
    if (cf == file_descriptor_format()) {
      return (format->tymed & TYMED_HGLOBAL) != 0 ? S_OK : DV_E_TYMED;
    }
    if (cf == file_contents_format()) {
      return (format->tymed & (TYMED_ISTREAM | TYMED_HGLOBAL)) != 0
                 ? S_OK
                 : DV_E_TYMED;
    }
    return DV_E_FORMATETC;
  }

  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*,
                                                  FORMATETC* output) override {
    if (output != nullptr) output->ptd = nullptr;
    return DATA_S_SAMEFORMATETC;
  }

  HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*,
                                    BOOL) override {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction,
                                          IEnumFORMATETC** output) override {
    if (direction != DATADIR_GET || output == nullptr) return E_NOTIMPL;
    FORMATETC formats[3] = {};
    for (FORMATETC& format : formats) {
      format.dwAspect = DVASPECT_CONTENT;
      format.lindex = -1;
      format.ptd = nullptr;
    }
    UINT count = 0;
    if (!streaming_) {
      formats[count].cfFormat = static_cast<CLIPFORMAT>(CF_HDROP);
      formats[count].tymed = TYMED_HGLOBAL;
      ++count;
    }
    formats[count].cfFormat =
        static_cast<CLIPFORMAT>(file_descriptor_format());
    formats[count].tymed = TYMED_HGLOBAL;
    ++count;
    formats[count].cfFormat =
        static_cast<CLIPFORMAT>(file_contents_format());
    formats[count].tymed = TYMED_ISTREAM;
    ++count;
    return SHCreateStdEnumFmtEtc(count, formats, output);
  }

  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*,
                                    DWORD*) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }

 private:
  HRESULT get_hdrop(STGMEDIUM* medium) {
    std::vector<std::string> paths;
    if (!offer_->source || !offer_->source->ensure_all(paths)) {
      return STG_E_READFAULT;
    }
    HGLOBAL memory = make_hdrop(paths);
    if (memory == nullptr) return STG_E_MEDIUMFULL;
    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = memory;
    medium->pUnkForRelease = nullptr;
    return S_OK;
  }

  HRESULT get_descriptor(STGMEDIUM* medium) {
    HGLOBAL memory = make_file_descriptor(offer_->files);
    if (memory == nullptr) return STG_E_MEDIUMFULL;
    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = memory;
    medium->pUnkForRelease = nullptr;
    return S_OK;
  }

  HRESULT get_stream(std::size_t index, STGMEDIUM* medium) {
    if (streaming_) {
      IStream* stream = remote_file_stream::open(offer_->source, index);
      if (stream == nullptr) return STG_E_READFAULT;
      medium->tymed = TYMED_ISTREAM;
      medium->pstm = stream;
      medium->pUnkForRelease = nullptr;
      return S_OK;
    }
    std::string path;
    if (!offer_->source ||
        !offer_->source->ensure(index, path)) {
      return STG_E_READFAULT;
    }
    std::string error;
    IStream* stream = lazy_file_stream::open(path, error);
    if (stream == nullptr) return STG_E_READFAULT;
    medium->tymed = TYMED_ISTREAM;
    medium->pstm = stream;
    medium->pUnkForRelease = nullptr;
    return S_OK;
  }

  /// Some consumers ask for FileContents as a single HGLOBAL even though a
  /// stream is the documented medium. Support it for modest files only.
  HRESULT get_contents(std::size_t index, STGMEDIUM* medium) {
    constexpr uint64_t kMaxGlobalContents = 64ull * 1024 * 1024;
    if (offer_->files[index].size > kMaxGlobalContents) return DV_E_TYMED;
    if (streaming_) return get_stream_contents(index, medium);
    std::string path;
    if (!offer_->source || !offer_->source->ensure(index, path)) {
      return STG_E_READFAULT;
    }
    const std::wstring wide = to_wide(path);
    HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return STG_E_READFAULT;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) {
      CloseHandle(file);
      return STG_E_READFAULT;
    }
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(size.QuadPart));
    if (memory == nullptr) {
      CloseHandle(file);
      return STG_E_MEDIUMFULL;
    }
    auto* target = static_cast<BYTE*>(GlobalLock(memory));
    DWORD total = 0;
    bool ok = target != nullptr;
    while (ok && total < static_cast<DWORD>(size.QuadPart)) {
      DWORD done = 0;
      ok = ::ReadFile(file, target + total,
                      static_cast<DWORD>(size.QuadPart) - total, &done,
                      nullptr) != 0;
      total += done;
      if (done == 0) break;
    }
    if (target != nullptr) GlobalUnlock(memory);
    CloseHandle(file);
    if (!ok || target == nullptr) {
      GlobalFree(memory);
      return STG_E_READFAULT;
    }
    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = memory;
    medium->pUnkForRelease = nullptr;
    return S_OK;
  }

  /// Some consumers ask for FileContents with TYMED_HGLOBAL even in streaming
  /// mode. Serve modest files by reading the virtual stream into memory; the
  /// 64 MiB cap above keeps a paste from allocating a whole video.
  HRESULT get_stream_contents(std::size_t index, STGMEDIUM* medium) {
    const uint64_t size = offer_->files[index].size;
    IStream* stream = remote_file_stream::open(offer_->source, index);
    if (stream == nullptr) return STG_E_READFAULT;
    HGLOBAL memory =
        GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(size == 0 ? 1 : size));
    if (memory == nullptr) {
      stream->Release();
      return STG_E_MEDIUMFULL;
    }
    auto* target = static_cast<BYTE*>(GlobalLock(memory));
    if (target == nullptr) {
      GlobalFree(memory);
      stream->Release();
      return STG_E_MEDIUMFULL;
    }
    ULONG total = 0;
    bool ok = true;
    while (total < size) {
      ULONG done = 0;
      const HRESULT result =
          stream->Read(target + total, static_cast<ULONG>(size - total), &done);
      if (FAILED(result) || done == 0) {
        ok = false;
        break;
      }
      total += done;
    }
    GlobalUnlock(memory);
    stream->Release();
    if (!ok) {
      GlobalFree(memory);
      return STG_E_READFAULT;
    }
    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = memory;
    medium->pUnkForRelease = nullptr;
    return S_OK;
  }

  std::atomic<ULONG> references_{1};
  std::shared_ptr<const lazy_offer> offer_;
  bool streaming_ = false;
};

/// State shared with the message-only window created by the lazy thread.
struct lazy_window_state {
  std::shared_ptr<const lazy_offer> offer;
};

lazy_window_state* window_state(HWND window) {
  return reinterpret_cast<lazy_window_state*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
}

LRESULT CALLBACK lazy_window_proc(HWND window, UINT message, WPARAM wparam,
                                  LPARAM lparam) {
  if (message == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    return DefWindowProcW(window, message, wparam, lparam);
  }
  lazy_window_state* state = window_state(window);
  switch (message) {
    case WM_RENDERFORMAT: {
      if (state == nullptr || state->offer == nullptr ||
          state->offer->source == nullptr) {
        return 0;
      }
      if (state->offer->source->streaming()) return 0;
      if (static_cast<UINT>(wparam) == CF_HDROP) {
        std::vector<std::string> paths;
        if (!state->offer->source->ensure_all(paths)) return 0;
        HGLOBAL memory = make_hdrop(paths);
        if (memory != nullptr) {
          // WM_RENDERFORMAT runs with the clipboard open; SetClipboardData
          // transfers ownership of the handle.
          SetClipboardData(CF_HDROP, memory);
        }
      }
      return 0;
    }
    case WM_RENDERALLFORMATS: {
      // The owner is shutting down (or another app called EmptyClipboard with
      // rendering). Render the one delayed format we advertise.
      if (state != nullptr && state->offer != nullptr &&
          state->offer->source != nullptr &&
          !state->offer->source->streaming()) {
        std::vector<std::string> paths;
        if (state->offer->source->ensure_all(paths)) {
          HGLOBAL memory = make_hdrop(paths);
          if (memory != nullptr) SetClipboardData(CF_HDROP, memory);
        }
      }
      return 0;
    }
    case WM_DESTROYCLIPBOARD:
      return 0;
    case kLazyStopMessage:
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(window, message, wparam, lparam);
  }
}

class windows_clipboard final : public clipboard_service {
 public:
  windows_clipboard() : owner_window_(create_clipboard_owner_window()) {}

  ~windows_clipboard() override {
    stop_watch();
    stop_lazy();
    if (owner_window_ != nullptr) {
      DestroyWindow(owner_window_);
      owner_window_ = nullptr;
    }
  }

  bool available() const override { return true; }
  std::string backend() const override { return "win32-clipboard"; }

  /// Polls GetClipboardSequenceNumber (cheap; no window needed) and reports
  /// the new text. This is what pushes what the host user copied to the
  /// active viewer, so the clipboard behaves like RDP redirection in both
  /// directions. The owner is reported with the value: a change this process
  /// made is logged and dropped instead of being sent back.
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
        // Classify before opening the clipboard: our own OpenClipboard would
        // otherwise make the probe name this process.
        const clipboard_owner owner = classify_clipboard_owner(owner_window_);
        std::string text;
        request_text([&text](std::string value) { text = std::move(value); });
        if (!text.empty()) {
          // GetClipboardSequenceNumber already proves a new SetClipboardData
          // happened. The content is deliberately not compared with the
          // previous value: copying the same password twice is two copies and
          // the second one has to reach the viewer. Echo suppression is the
          // caller's job (owner == self, or its own recent write).
          std::lock_guard<std::mutex> lock(watch_mutex_);
          last_files_.clear();
          if (watch_callback_) watch_callback_(text, owner);
        }

        // A copy may carry files, text, or both. A text-only copy skips the
        // OLE describe (it can take seconds); a copy with CF_HDROP or a file
        // descriptor is inspected even when it also has text, so both formats
        // reach the viewer.
        if (!text.empty() && !clipboard_may_hold_files()) continue;
        std::vector<clipboard_file> files;
        request_files([&files](std::vector<clipboard_file> value) {
          files = std::move(value);
        });
        if (files.empty()) continue;
        std::string signature;
        for (const clipboard_file& file : files) {
          if (!file.path.empty()) {
            signature += file.path;
          } else {
            // Inner RDP virtual file: the path is intentionally empty, so the
            // name/size/generation pair is what makes a new copy visible.
            signature += "@stream:";
            signature += file.name;
            signature += ':';
            signature += std::to_string(file.size);
            signature += ':';
            signature += std::to_string(file.stream_id);
          }
          signature += '\n';
        }
        std::lock_guard<std::mutex> lock(watch_mutex_);
        if (signature == last_files_) continue;
        last_files_ = signature;
        if (files_callback_) files_callback_(std::move(files), owner);
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
    // A text copy replaces any delayed file offer this process owned. Clear
    // the source first so a render thread waiting on it wakes immediately.
    clear_lazy_registry("text clipboard replaced the file offer");
    stop_lazy();
    constexpr std::size_t kMaxText = 192 * 1024;
    if (text.size() > kMaxText) text.resize(kMaxText);
    const std::wstring wide = to_wide(text);

    for (int attempt = 0; attempt < 5; ++attempt) {
      // Name this process as the owner so the watcher (and another remote
      // tool reading the clipboard) can tell our write from a local copy.
      if (!OpenClipboard(owner_window_)) {
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
    // Never force a delayed render: the host's own watcher runs four times a
    // second and would download the whole offer before anyone pastes.
    if (lazy_hiding_or_lost()) {
      if (callback) callback(std::string());
      return;
    }
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
    // This process published a virtual offer for the remote clipboard. Its
    // descriptors describe files that were just copied on the viewer, not a
    // local selection; enumerating them would send them straight back.
    if (windows_virtual_clipboard_owns_clipboard()) {
      if (callback) callback({});
      return;
    }
    if (lazy_hiding_or_lost()) {
      std::vector<clipboard_file> files;
      const std::shared_ptr<const lazy_offer> offer = lazy_snapshot();
      if (offer != nullptr) {
        files.reserve(offer->files.size());
        for (const lazy_clipboard_file& file : offer->files) {
          clipboard_file entry;
          entry.name = file.name;
          entry.size = file.size;
          // No path: the bytes do not exist yet. Clients use the empty path to
          // tell a delayed offer from a real Explorer selection.
          files.push_back(std::move(entry));
        }
      }
      if (callback) callback(std::move(files));
      return;
    }
    std::vector<clipboard_file> files;
    std::string source;
    // The reader answers both kinds of copy: FileGroupDescriptorW +
    // FileContents for an inner RDP session, CF_HDROP for an ordinary local
    // Explorer selection. It keeps them apart with `source`.
    if (reader_.describe(files, kObjectDescribeTimeoutMs, &source)) {
      clipboard_log("request_files source=%s count=%zu", source.c_str(),
                    files.size());
      if (callback) callback(std::move(files));
      return;
    }
    // The worker can be busy inside a blocked RDP stream, or wedged after one
    // timed out. Read CF_HDROP directly in that case rather than reporting an
    // empty clipboard.
    hdrop_scan_result scan;
    if (scan_hdrop(nullptr, scan)) {
      clipboard_path_build built = build_clipboard_files(scan.entries);
      files = std::move(built.files);
      if (built.invalid > 0 || scan.truncated || scan.unreadable > 0) {
        clipboard_log(
            "request_files hdrop invalid=%zu unreadable=%zu truncated=%d",
            built.invalid, scan.unreadable, scan.truncated ? 1 : 0);
      }
    }
    clipboard_log("request_files source=%s count=%zu",
                  files.empty() ? "none" : "hdrop", files.size());
    if (callback) callback(std::move(files));
  }

  /// A `clipboard-pull` for an entry that came from the OLE data object. The
  /// read runs on the data-object thread, never here.
  void read_file_range(const clipboard_file& file, uint64_t offset,
                       uint64_t length,
                       file_range_callback callback) override {
    file_range_callback fallback = callback;
    try {
      reader_.read(file, offset, length, std::move(callback),
                   static_cast<uint32_t>(kObjectReadTimeoutMs),
                   static_cast<uint32_t>(kObjectReadTotalMs));
    } catch (const std::exception& error) {
      clipboard_log("data-object read start failed: %s", error.what());
      if (fallback) {
        try {
          fallback(2, offset, {});
        } catch (...) {
        }
      }
    } catch (...) {
      clipboard_log("data-object read start failed");
      if (fallback) {
        try {
          fallback(2, offset, {});
        } catch (...) {
        }
      }
    }
  }

  bool set_files(const std::vector<std::string>& paths) override {
    clear_lazy_registry("real files replaced the delayed offer");
    stop_lazy();
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
      if (!OpenClipboard(owner_window_)) {
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

  bool supports_lazy_files() const override { return true; }

  /// An inner RDP session owns the clipboard when mstsc/msrdc wrote the
  /// current contents (copying a file inside the remote desktop materialises
  /// it on this side and makes the RDP client the owner), or when CF_HDROP
  /// points into the client's redirection temp folder. Cached for a second:
  /// this is called from the supervisor's clipboard poll and from the offer
  /// handler, and it opens the clipboard.
  bool nested_rdp() const override {
    const uint64_t now = GetTickCount64();
    {
      std::lock_guard<std::mutex> lock(nested_mutex_);
      if (nested_checked_ && now - nested_checked_ms_ < 1000) {
        return nested_cached_;
      }
    }
    bool nested = false;
    if (open_clipboard_retry(nullptr, 3)) {
      const HWND owner = GetClipboardOwner();
      if (owner != nullptr) {
        DWORD process_id = 0;
        GetWindowThreadProcessId(owner, &process_id);
        if (process_id != 0 && process_id != GetCurrentProcessId()) {
          HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                       FALSE, process_id);
          if (process != nullptr) {
            wchar_t path[MAX_PATH] = {};
            DWORD length = MAX_PATH;
            if (QueryFullProcessImageNameW(process, 0, path, &length)) {
              const std::wstring name =
                  std::filesystem::path(path).filename().wstring();
              std::wstring lower = name;
              std::transform(lower.begin(), lower.end(), lower.begin(),
                             [](wchar_t c) {
                               return static_cast<wchar_t>(std::towlower(c));
                             });
              nested = lower == L"mstsc.exe" || lower == L"msrdc.exe";
            }
            CloseHandle(process);
          }
        }
      }
      if (!nested) {
        // The clipboard owner check only fires after the inner session copied
        // something. A connected mstsc window is the earlier signal.
        bool rdp_window = false;
        EnumWindows(&find_rdp_window, reinterpret_cast<LPARAM>(&rdp_window));
        nested = rdp_window;
      }
      if (!nested && IsClipboardFormatAvailable(CF_HDROP)) {
        HANDLE data = GetClipboardData(CF_HDROP);
        if (data != nullptr) {
          HDROP drop = static_cast<HDROP>(data);
          const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
          for (UINT i = 0; i < count && !nested; ++i) {
            const UINT length = DragQueryFileW(drop, i, nullptr, 0);
            if (length == 0) continue;
            std::wstring path(length + 1, L'\0');
            if (DragQueryFileW(drop, i, path.data(), length + 1) == 0) {
              continue;
            }
            path.resize(length);
            std::wstring lower = path;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](wchar_t c) {
                             return static_cast<wchar_t>(std::towlower(c));
                           });
            // mstsc materialises redirected files as RDP<hex> temp names, and
            // an explicit \\tsclient path is the other shape we see.
            nested = lower.find(L"\\\\tsclient\\") == 0 ||
                     lower.find(L"\\temp\\rdp") != std::wstring::npos ||
                     lower.find(L"/temp/rdp") != std::wstring::npos;
          }
        }
      }
      CloseClipboard();
    }
    {
      std::lock_guard<std::mutex> lock(nested_mutex_);
      nested_cached_ = nested;
      nested_checked_ = true;
      nested_checked_ms_ = now;
    }
    return nested;
  }

  bool install_lazy(std::shared_ptr<lazy_clipboard> lazy,
                    std::string& error) override {
    if (!lazy) {
      error = "no lazy clipboard registry";
      return false;
    }
    const std::shared_ptr<const lazy_offer> offer = lazy->snapshot();
    if (!offer || !offer->source) {
      error = "no clipboard offer to render";
      return false;
    }
    stop_lazy();
    {
      std::lock_guard<std::mutex> lock(lazy_mutex_);
      lazy_registry_ = std::move(lazy);
      lazy_offer_ = offer;
      lazy_ready_ = false;
      lazy_ok_ = false;
      lazy_ole_ = false;
      lazy_lost_ = false;
      lazy_hwnd_ = nullptr;
      lazy_sequence_ = 0;
      lazy_error_.clear();
    }
    lazy_thread_ = std::thread([this] { run_lazy_thread(); });
    std::unique_lock<std::mutex> lock(lazy_mutex_);
    if (!lazy_cv_.wait_for(lock, std::chrono::seconds(5),
                           [this] { return lazy_ready_; })) {
      error = "the clipboard render thread did not start";
      lock.unlock();
      stop_lazy();
      return false;
    }
    if (!lazy_ok_) {
      error = lazy_error_.empty() ? "could not own the clipboard" : lazy_error_;
      lock.unlock();
      stop_lazy();
      return false;
    }
    return true;
  }

  void uninstall_lazy() override { stop_lazy(); }

  bool lazy_rendering() const override { return lazy_hiding(); }

 private:
  /// True while this process owns the OS clipboard for a lazy offer. The
  /// sequence number is the ownership witness: any app that replaces the
  /// clipboard bumps it, and our offer must stop serving.
  bool lazy_hiding() const {
    std::lock_guard<std::mutex> lock(lazy_mutex_);
    if (lazy_lost_ || lazy_hwnd_ == nullptr || lazy_sequence_ == 0 ||
        lazy_offer_ == nullptr) {
      return false;
    }
    if (GetClipboardSequenceNumber() != lazy_sequence_) {
      // The clipboard changed. Do not clear here (this may be the watcher
      // thread and the offer may belong to a live session); mark the offer
      // unusable so a render cannot serve stale data. The next install or
      // session close releases the thread.
      lazy_lost_ = true;
      return false;
    }
    return true;
  }

  /// Same test, but also clears the registry when ownership was lost, so
  /// waiting render threads fail instead of blocking until the timeout.
  bool lazy_hiding_or_lost() {
    if (lazy_hiding()) return true;
    std::shared_ptr<lazy_clipboard> registry;
    bool lost = false;
    {
      std::lock_guard<std::mutex> lock(lazy_mutex_);
      lost = lazy_lost_;
      registry = lazy_registry_;
    }
    if (lost && registry) registry->clear_all("host clipboard changed");
    return false;
  }

  std::shared_ptr<const lazy_offer> lazy_snapshot() const {
    std::lock_guard<std::mutex> lock(lazy_mutex_);
    return lazy_offer_;
  }

  void clear_lazy_registry(const std::string& reason) {
    std::shared_ptr<lazy_clipboard> registry;
    {
      std::lock_guard<std::mutex> lock(lazy_mutex_);
      registry = lazy_registry_;
    }
    if (registry) registry->clear_all(reason);
  }

  void run_lazy_thread();
  void stop_lazy();

  std::mutex watch_mutex_;
  text_watcher watch_callback_;
  files_watcher files_callback_;
  std::string last_files_;
  DWORD sequence_ = 0;
  std::atomic<bool> running_{false};
  std::thread watcher_;

  /// Message-only window that names this process as the clipboard owner for
  /// the eager text/file writes. GetClipboardOwner() returning it (or any
  /// window in this process, which covers the OLE lazy path) makes a change
  /// an echo rather than a local copy.
  HWND owner_window_ = nullptr;

  /// OLE data-object reader (inner RDP virtual files). Declared after the
  /// watcher so it starts only once this object is fully constructed; its
  /// destructor runs after the watcher thread has been joined.
  clipboard_object_reader reader_;

  /// nested_rdp() cache; the probe opens the clipboard and inspects the owner
  /// process, so the supervisor's 400 ms poll must not pay it every tick.
  mutable std::mutex nested_mutex_;
  mutable bool nested_cached_ = false;
  mutable bool nested_checked_ = false;
  mutable uint64_t nested_checked_ms_ = 0;

  mutable std::mutex lazy_mutex_;
  std::condition_variable lazy_cv_;
  std::shared_ptr<lazy_clipboard> lazy_registry_;
  std::shared_ptr<const lazy_offer> lazy_offer_;
  HWND lazy_hwnd_ = nullptr;
  DWORD lazy_sequence_ = 0;
  bool lazy_ready_ = false;
  bool lazy_ok_ = false;
  bool lazy_ole_ = false;
  // Set by lazy_hiding() (const) when another app replaced the clipboard.
  mutable bool lazy_lost_ = false;
  std::string lazy_error_;
  std::thread lazy_thread_;
};

void windows_clipboard::run_lazy_thread() {
  std::shared_ptr<const lazy_offer> offer;
  {
    std::lock_guard<std::mutex> lock(lazy_mutex_);
    offer = lazy_offer_;
  }
  const HRESULT ole_result = OleInitialize(nullptr);
  const bool ole_available = SUCCEEDED(ole_result);

  WNDCLASSEXW window_class = {};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = lazy_window_proc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.lpszClassName = kLazyWindowClass;
  RegisterClassExW(&window_class);  // already registered is fine

  auto* state = new lazy_window_state{offer};
  HWND window = CreateWindowExW(0, kLazyWindowClass, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr,
                                GetModuleHandleW(nullptr), state);
  if (window == nullptr) {
    delete state;
    {
      std::lock_guard<std::mutex> lock(lazy_mutex_);
      lazy_ready_ = true;
      lazy_ok_ = false;
      lazy_error_ = "could not create the clipboard window";
    }
    lazy_cv_.notify_all();
    if (ole_available) OleUninitialize();
    return;
  }

  const bool streaming =
      offer && offer->source && offer->source->streaming();
  bool ole_mode = false;
  bool installed = false;
  std::string install_error;
  if (ole_available) {
    auto* object = new lazy_data_object(offer);
    for (int attempt = 0; attempt < 10 && !installed; ++attempt) {
      const HRESULT result = OleSetClipboard(object);
      if (SUCCEEDED(result)) {
        ole_mode = true;
        installed = true;
      } else {
        Sleep(10);
      }
    }
    object->Release();
    if (!installed) install_error = "could not own the host clipboard";
  } else if (streaming) {
    install_error = "virtual files need OLE clipboard support";
  }
  if (!installed && !streaming) {
    // Classic delayed rendering: CF_HDROP is declared with a null handle and
    // rendered in WM_RENDERFORMAT. FileGroupDescriptorW is supplied
    // immediately so applications can enumerate the offer. FileContents only
    // exists in the OLE model above, so classic mode serves Explorer's
    // CF_HDROP paste path only -- and it is impossible in streaming mode,
    // which must never stage the bytes.
    if (open_clipboard_retry(window)) {
      EmptyClipboard();
      HGLOBAL descriptor = make_file_descriptor(offer->files);
      if (descriptor != nullptr) {
        if (SetClipboardData(file_descriptor_format(), descriptor) == nullptr) {
          GlobalFree(descriptor);
        }
      }
      // A null handle declares delayed rendering: the system asks us with
      // WM_RENDERFORMAT the first time an application reads CF_HDROP.
      if (SetClipboardData(CF_HDROP, nullptr) != nullptr) installed = true;
      CloseClipboard();
    }
  }

  {
    std::lock_guard<std::mutex> lock(lazy_mutex_);
    lazy_hwnd_ = window;
    lazy_ole_ = ole_mode;
    lazy_sequence_ =
        installed ? GetClipboardSequenceNumber() : 0;
    lazy_ready_ = true;
    lazy_ok_ = installed;
    if (!installed) {
      lazy_error_ = install_error.empty() ? "could not own the host clipboard"
                                          : install_error;
    }
  }
  lazy_cv_.notify_all();
  if (!installed) {
    DestroyWindow(window);
    delete state;
    if (ole_available) OleUninitialize();
    return;
  }

  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  bool owned = false;
  bool ole_owned = false;
  {
    std::lock_guard<std::mutex> lock(lazy_mutex_);
    owned = lazy_sequence_ != 0 &&
            GetClipboardSequenceNumber() == lazy_sequence_;
    ole_owned = lazy_ole_;
    lazy_hwnd_ = nullptr;
    lazy_sequence_ = 0;
  }
  if (owned) {
    if (ole_owned) {
      OleSetClipboard(nullptr);
    } else if (open_clipboard_retry(window)) {
      EmptyClipboard();
      CloseClipboard();
    }
  }
  DestroyWindow(window);
  delete state;
  UnregisterClassW(kLazyWindowClass, GetModuleHandleW(nullptr));
  if (ole_available) OleUninitialize();
}

void windows_clipboard::stop_lazy() {
  std::thread thread;
  HWND window = nullptr;
  std::shared_ptr<const lazy_offer> served;
  {
    std::lock_guard<std::mutex> lock(lazy_mutex_);
    thread = std::move(lazy_thread_);
    window = lazy_hwnd_;
    served = lazy_offer_;
  }
  // Wake a render blocked on the offer this thread served. Do not clear the
  // registry: the caller may already have installed a newer offer in the same
  // host-global registry.
  if (served && served->source) {
    served->source->fail_all("lazy clipboard released");
  }
  if (thread.joinable()) {
    if (window != nullptr) PostMessageW(window, kLazyStopMessage, 0, 0);
    thread.join();
  }
  std::lock_guard<std::mutex> lock(lazy_mutex_);
  lazy_hwnd_ = nullptr;
  lazy_registry_.reset();
  lazy_offer_.reset();
  lazy_sequence_ = 0;
  lazy_ready_ = false;
  lazy_ok_ = false;
  lazy_ole_ = false;
  lazy_lost_ = false;
}

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
