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
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "clipboard_lazy.h"

namespace molink {
namespace {

constexpr UINT kLazyStopMessage = WM_APP + 0x51;
constexpr wchar_t kLazyWindowClass[] = L"moLinkLazyClipboard";

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

/// mstsc's main window class. Its presence means an inner Windows session is
/// connected; clipboard redirection then only understands CF_HDROP.
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

UINT file_descriptor_format() {
  static const UINT format =
      RegisterClipboardFormatW(L"FileGroupDescriptorW");
  return format;
}

UINT file_contents_format() {
  static const UINT format = RegisterClipboardFormatW(L"FileContents");
  return format;
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
  ~windows_clipboard() override {
    stop_watch();
    stop_lazy();
  }

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
    // A text copy replaces any delayed file offer this process owned. Clear
    // the source first so a render thread waiting on it wakes immediately.
    clear_lazy_registry("text clipboard replaced the file offer");
    stop_lazy();
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
            const std::filesystem::path fs_path(path);
            std::error_code ec;
            // CF_HDROP can name folders too. A recursive folder copy is the
            // files panel's job; the clipboard file list only carries regular
            // files, and a folder here would make a pull fail.
            if (!std::filesystem::is_regular_file(fs_path, ec) || ec) continue;
            clipboard_file file;
            file.path = to_utf8(path.c_str());
            const std::wstring name = fs_path.filename().wstring();
            file.name = to_utf8(name.c_str());
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
  std::string last_text_;
  std::string last_files_;
  DWORD sequence_ = 0;
  std::atomic<bool> running_{false};
  std::thread watcher_;

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
