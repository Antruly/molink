// Virtual-file clipboard publisher for Windows.
//
// This is the viewer-side counterpart of windows_data_object.cpp: instead of
// reading an inner RDP client's FileContents streams, this file *offers* the
// remote clipboard's files as FileContents streams. Explorer drives the copy
// when the local user pastes; each IStream::Read becomes one 256 KiB
// `clipboard-pull` range over the existing session. Nothing is downloaded when
// the remote user copies, and Explorer draws its own progress dialog.
//
// The object is published with OleSetClipboard and never flushed: flushing
// would materialise every byte before the user even pasted.

#include "clipboard/windows_virtual_clipboard.h"

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
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "clipboard/windows_data_object.h"

namespace molink {
namespace {

constexpr wchar_t kVirtualWindowClass[] = L"moLinkVirtualClipboard";
constexpr UINT kVirtualStopMessage = WM_APP + 0x61;
constexpr UINT_PTR kOwnershipTimerId = 1;
/// How often the publisher notices that another application replaced the
/// clipboard. A paste that started before that already fails through
/// cancel_reads(); this bounds the window in which a newly pasted stream can
/// still try to pull from a dead offer.
constexpr UINT kOwnershipPollMs = 500;

std::wstring to_wide(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr,
                                         0);
  if (needed <= 0) return std::wstring();
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      wide.data(), needed);
  return wide;
}

/// Process-wide ownership witness. The sequence number is what makes a stale
/// offer distinguishable from a fresh one: any app that replaces the clipboard
/// bumps it, so the watcher stops treating our object as the current owner.
std::mutex& owner_mutex() {
  static std::mutex mutex;
  return mutex;
}
DWORD& owner_sequence() {
  static DWORD sequence = 0;
  return sequence;
}
bool& owner_active() {
  static bool active = false;
  return active;
}

void note_clipboard_owner(DWORD sequence) {
  std::lock_guard<std::mutex> lock(owner_mutex());
  owner_sequence() = sequence;
  owner_active() = sequence != 0;
}

void release_clipboard_owner(DWORD sequence) {
  std::lock_guard<std::mutex> lock(owner_mutex());
  if (owner_sequence() != sequence) return;
  owner_active() = false;
  owner_sequence() = 0;
}

HGLOBAL make_file_descriptor(const std::vector<virtual_mount_file>& files) {
  if (files.empty()) return nullptr;
  const std::size_t bytes =
      sizeof(UINT) + files.size() * sizeof(FILEDESCRIPTORW);
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

/// IStream over one advertised file. Explorer reads sequentially (and seeks
/// to the start); a range failure is returned as STG_E_READFAULT so the copy
/// dialog marks that file as failed instead of writing a truncated copy.
class virtual_file_stream final : public IStream {
 public:
  static IStream* open(std::shared_ptr<virtual_stream_source> source,
                       std::size_t index) {
    if (!source || index >= source->size()) return nullptr;
    return new virtual_file_stream(std::move(source), index);
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
    if (buffer == nullptr && bytes != 0) return STG_E_INVALIDPOINTER;
    const virtual_stream_read_result result = cursor_.read(buffer, bytes);
    if (result.status == virtual_stream_status::read_error ||
        result.status == virtual_stream_status::stopped) {
      return STG_E_READFAULT;
    }
    if (read != nullptr) *read = result.bytes;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override {
    return STG_E_ACCESSDENIED;
  }

  HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin,
                                 ULARGE_INTEGER* new_position) override {
    virtual_stream_seek_origin kind = virtual_stream_seek_origin::begin;
    if (origin == STREAM_SEEK_SET) {
      kind = virtual_stream_seek_origin::begin;
    } else if (origin == STREAM_SEEK_CUR) {
      kind = virtual_stream_seek_origin::current;
    } else if (origin == STREAM_SEEK_END) {
      kind = virtual_stream_seek_origin::end;
    } else {
      return STG_E_INVALIDFUNCTION;
    }
    uint64_t target = 0;
    if (!cursor_.seek(move.QuadPart, kind, target)) {
      return STG_E_INVALIDFUNCTION;
    }
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
    stat->cbSize.QuadPart = cursor_.size();
    stat->grfMode = STGM_READ;
    stat->grfLocksSupported = 0;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Clone(IStream** output) override {
    if (output == nullptr) return E_INVALIDARG;
    *output = nullptr;
    auto* clone =
        new virtual_file_stream(source_, index_);
    const uint64_t position = cursor_.position();
    if (position != 0) {
      uint64_t ignored = 0;
      clone->cursor_.seek(static_cast<int64_t>(position),
                          virtual_stream_seek_origin::begin, ignored);
    }
    *output = clone;
    return S_OK;
  }

 private:
  virtual_file_stream(std::shared_ptr<virtual_stream_source> source,
                      std::size_t index)
      : source_(std::move(source)),
        index_(index),
        cursor_(source_, index_) {}

  std::atomic<ULONG> references_{1};
  std::shared_ptr<virtual_stream_source> source_;
  std::size_t index_ = 0;
  virtual_stream_cursor cursor_;
};

class virtual_file_data_object final : public IDataObject {
 public:
  explicit virtual_file_data_object(
      std::shared_ptr<virtual_stream_source> source)
      : source_(std::move(source)) {}

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
    if (cf == file_descriptor_format()) {
      if ((format->tymed & TYMED_HGLOBAL) == 0) return DV_E_TYMED;
      HGLOBAL memory = make_file_descriptor(source_->files());
      if (memory == nullptr) return STG_E_MEDIUMFULL;
      medium->tymed = TYMED_HGLOBAL;
      medium->hGlobal = memory;
      medium->pUnkForRelease = nullptr;
      return S_OK;
    }
    if (cf == file_contents_format()) {
      long index = format->lindex;
      if (index < 0 && source_->size() == 1) index = 0;
      if (index < 0 ||
          static_cast<std::size_t>(index) >= source_->size()) {
        return DV_E_LINDEX;
      }
      if ((format->tymed & TYMED_ISTREAM) == 0) {
        // Materialising a file as HGLOBAL before the user pastes is exactly
        // the eager behaviour this offer exists to avoid.
        return DV_E_TYMED;
      }
      IStream* stream =
          virtual_file_stream::open(source_, static_cast<std::size_t>(index));
      if (stream == nullptr) return STG_E_READFAULT;
      medium->tymed = TYMED_ISTREAM;
      medium->pstm = stream;
      medium->pUnkForRelease = nullptr;
      return S_OK;
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
    if (cf == file_descriptor_format()) {
      return (format->tymed & TYMED_HGLOBAL) != 0 ? S_OK : DV_E_TYMED;
    }
    if (cf == file_contents_format()) {
      long index = format->lindex;
      if (index < 0 && source_->size() == 1) index = 0;
      if (index < 0 ||
          static_cast<std::size_t>(index) >= source_->size()) {
        return DV_E_LINDEX;
      }
      return (format->tymed & TYMED_ISTREAM) != 0 ? S_OK : DV_E_TYMED;
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
    FORMATETC formats[2] = {};
    for (FORMATETC& format : formats) {
      format.dwAspect = DVASPECT_CONTENT;
      format.lindex = -1;
      format.ptd = nullptr;
    }
    formats[0].cfFormat =
        static_cast<CLIPFORMAT>(file_descriptor_format());
    formats[0].tymed = TYMED_HGLOBAL;
    formats[1].cfFormat = static_cast<CLIPFORMAT>(file_contents_format());
    formats[1].tymed = TYMED_ISTREAM;
    return SHCreateStdEnumFmtEtc(2, formats, output);
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
  std::atomic<ULONG> references_{1};
  std::shared_ptr<virtual_stream_source> source_;
};

struct mount_state {
  std::mutex mutex;
  std::condition_variable cv;
  bool ready = false;
  bool ok = false;
  std::string error;
  std::shared_ptr<virtual_stream_source> source;
  virtual_file_reader* reader = nullptr;
  HWND window = nullptr;
  DWORD thread_id = 0;
  DWORD sequence = 0;
  std::atomic<bool> lost{false};
};

/// The publisher runs on its own STA with a message loop, the same model the
/// host-side reader uses. IStream::Read may block on the bridge, and that must
/// never happen on the OLE owner thread or the core's event loop.
class windows_virtual_mount final : public virtual_mount {
 public:
  windows_virtual_mount(std::string mountpoint,
                        std::vector<virtual_mount_file> files,
                        virtual_file_reader* reader)
      : mountpoint_(std::move(mountpoint)),
        files_(std::move(files)),
        reader_(reader),
        state_(std::make_shared<mount_state>()) {
    state_->reader = reader_;
  }

  ~windows_virtual_mount() override { stop(); }

  windows_virtual_mount(const windows_virtual_mount&) = delete;
  windows_virtual_mount& operator=(const windows_virtual_mount&) = delete;

  bool start(std::string& error) {
    source_ = std::make_shared<virtual_stream_source>(files_, reader_);
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->source = source_;
    }
    thread_ = std::thread([this] { run(); });
    std::unique_lock<std::mutex> lock(state_->mutex);
    if (!state_->cv.wait_for(lock, std::chrono::seconds(5),
                             [this] { return state_->ready; })) {
      error = "the virtual clipboard thread did not start";
      lock.unlock();
      stop();
      return false;
    }
    if (!state_->ok) {
      error = state_->error.empty() ? "could not own the clipboard"
                                    : state_->error;
      return false;
    }
    mounted_.store(true);
    return true;
  }

  const std::string& path() const override { return mountpoint_; }
  bool mounted() const override { return mounted_.load(); }

  bool stop() override {
    if (stopped_.exchange(true)) return true;
    if (source_) source_->stop();
    if (reader_ != nullptr) reader_->cancel_reads();
    HWND window = nullptr;
    DWORD thread_id = 0;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      window = state_->window;
      thread_id = state_->thread_id;
    }
    if (thread_.joinable()) {
      // A thread message reaches the loop even when the window is already
      // gone. WM_QUIT itself is generated by PostQuitMessage only, so the
      // loop checks for this marker instead.
      if (thread_id != 0) {
        PostThreadMessageW(thread_id, kVirtualStopMessage, 0, 0);
      }
      if (window != nullptr) PostMessageW(window, kVirtualStopMessage, 0, 0);
      thread_.join();
    }
    mounted_.store(false);
    return true;
  }

 private:
  void run() {
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->thread_id = GetCurrentThreadId();
    }
    const HRESULT ole_result = OleInitialize(nullptr);
    const bool ole = SUCCEEDED(ole_result);
    std::string error;
    bool ok = ole;
    if (!ole) error = "virtual files need OLE clipboard support";

    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = kVirtualWindowClass;
    RegisterClassExW(&window_class);  // already registered is fine

    HWND window = nullptr;
    if (ok) {
      window = CreateWindowExW(0, kVirtualWindowClass, L"", 0, 0, 0, 0, 0,
                               HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr),
                               state_.get());
      if (window == nullptr) {
        ok = false;
        error = "could not create the virtual clipboard window";
      }
    }

    if (ok) {
      IDataObject* object = nullptr;
      const HRESULT built =
          create_virtual_file_data_object(state_->source, &object);
      if (FAILED(built) || object == nullptr) {
        ok = false;
        error = "could not build the virtual file data object";
      } else {
        bool installed = false;
        for (int attempt = 0; attempt < 10 && !installed; ++attempt) {
          const HRESULT result = OleSetClipboard(object);
          if (SUCCEEDED(result)) {
            installed = true;
          } else {
            Sleep(10);
          }
        }
        // OleSetClipboard took its own reference (or refused it); ours is
        // released either way.
        object->Release();
        if (!installed) {
          ok = false;
          error = "the clipboard is busy";
        } else {
          const DWORD sequence = GetClipboardSequenceNumber();
          note_clipboard_owner(sequence);
          {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->sequence = sequence;
          }
          SetTimer(window, kOwnershipTimerId, kOwnershipPollMs, nullptr);
        }
      }
    }

    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->window = ok ? window : nullptr;
      state_->ok = ok;
      state_->error = error;
      state_->ready = true;
    }
    state_->cv.notify_all();

    if (!ok) {
      if (window != nullptr) DestroyWindow(window);
      if (ole) OleUninitialize();
      return;
    }

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      if (message.message == kVirtualStopMessage) break;
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }

    DWORD sequence = 0;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      sequence = state_->sequence;
      state_->window = nullptr;
    }
    KillTimer(window, kOwnershipTimerId);
    // Never OleFlushClipboard: flushing renders every advertised stream. Only
    // release the object when it is still ours, so a newer clipboard owner is
    // left untouched.
    if (sequence != 0 && GetClipboardSequenceNumber() == sequence) {
      OleSetClipboard(nullptr);
    }
    release_clipboard_owner(sequence);
    DestroyWindow(window);
    UnregisterClassW(kVirtualWindowClass, GetModuleHandleW(nullptr));
    if (ole) OleUninitialize();
  }

  /// WM_TIMER checks whether another application replaced the clipboard. When
  /// it did, fail blocked reads and leave the message loop; a stream Explorer
  /// still holds then returns STG_E_READFAULT on its next read.
  static void check_ownership(mount_state* state) {
    if (state == nullptr || state->lost.load()) return;
    DWORD sequence = 0;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      sequence = state->sequence;
    }
    if (sequence == 0 || GetClipboardSequenceNumber() == sequence) return;
    state->lost.store(true);
    if (state->source) state->source->stop();
    if (state->reader != nullptr) state->reader->cancel_reads();
    PostQuitMessage(0);
  }

  static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                      WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
      auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      SetWindowLongPtrW(window, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(create->lpCreateParams));
      return DefWindowProcW(window, message, wparam, lparam);
    }
    auto* state = reinterpret_cast<mount_state*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_TIMER && wparam == kOwnershipTimerId) {
      check_ownership(state);
      return 0;
    }
    if (message == kVirtualStopMessage) {
      PostQuitMessage(0);
      return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
  }

  std::string mountpoint_;
  std::vector<virtual_mount_file> files_;
  virtual_file_reader* reader_ = nullptr;
  std::shared_ptr<mount_state> state_;
  std::shared_ptr<virtual_stream_source> source_;
  std::atomic<bool> mounted_{false};
  std::atomic<bool> stopped_{false};
  std::thread thread_;
};

}  // namespace

HRESULT create_virtual_file_data_object(
    std::shared_ptr<virtual_stream_source> source, IDataObject** out) {
  if (out == nullptr) return E_INVALIDARG;
  *out = nullptr;
  if (!source) return E_INVALIDARG;
  auto* object = new (std::nothrow) virtual_file_data_object(std::move(source));
  if (object == nullptr) return E_OUTOFMEMORY;
  *out = static_cast<IDataObject*>(object);
  return S_OK;
}

bool windows_virtual_clipboard_owns_clipboard() {
  std::lock_guard<std::mutex> lock(owner_mutex());
  if (!owner_active() || owner_sequence() == 0) return false;
  return GetClipboardSequenceNumber() == owner_sequence();
}

bool platform_virtual_mount_supported() { return true; }

bool platform_path_is_mountpoint(const std::string&) { return false; }

virtual_mount_factory platform_virtual_mount_factory() {
  return [](const std::string& mountpoint,
            const std::vector<virtual_mount_file>& files,
            virtual_file_reader* reader, std::string& error)
             -> std::shared_ptr<virtual_mount> {
    if (reader == nullptr) {
      error = "no file reader";
      return nullptr;
    }
    auto mount = std::make_shared<windows_virtual_mount>(mountpoint, files,
                                                         reader);
    if (!mount->start(error)) return nullptr;
    return mount;
  };
}

}  // namespace molink

#endif  // _WIN32
