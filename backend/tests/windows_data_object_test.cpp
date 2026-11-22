// Windows-only unit test for the OLE data-object reader used by the nested
// RDP clipboard path.
//
// A fake IDataObject plays mstsc: FileGroupDescriptorW names the files,
// FileContents answers per-index IStream reads, and CF_HDROP is present too so
// the test can prove the stream formats win. Nothing here touches the real
// clipboard, so unlike windows_clipboard_stream_test this runs unconditionally
// on a Windows build.

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

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "clipboard/windows_data_object.h"

namespace {

int checks = 0;
int failures = 0;

void expect(bool ok, const char* message) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAIL: %s\n", message);
  }
}

class fake_stream final : public IStream {
 public:
  explicit fake_stream(std::string data) : data_(std::move(data)) {}

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
    if (buffer == nullptr) return STG_E_INVALIDPOINTER;
    const std::size_t available =
        position_ < data_.size() ? data_.size() - position_ : 0;
    const std::size_t take =
        available < bytes ? available : static_cast<std::size_t>(bytes);
    if (take != 0) std::memcpy(buffer, data_.data() + position_, take);
    position_ += take;
    if (read != nullptr) *read = static_cast<ULONG>(take);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override {
    return STG_E_ACCESSDENIED;
  }
  HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin,
                                 ULARGE_INTEGER* position) override {
    LONGLONG base = 0;
    if (origin == STREAM_SEEK_SET) {
      base = 0;
    } else if (origin == STREAM_SEEK_CUR) {
      base = static_cast<LONGLONG>(position_);
    } else if (origin == STREAM_SEEK_END) {
      base = static_cast<LONGLONG>(data_.size());
    } else {
      return STG_E_INVALIDFUNCTION;
    }
    const LONGLONG target = base + move.QuadPart;
    if (target < 0) return STG_E_INVALIDFUNCTION;
    position_ = static_cast<std::size_t>(target);
    if (position != nullptr) position->QuadPart = position_;
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
    std::memset(stat, 0, sizeof(*stat));
    stat->type = STGTY_STREAM;
    stat->cbSize.QuadPart = data_.size();
    stat->grfMode = STGM_READ;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Clone(IStream** output) override {
    if (output == nullptr) return E_INVALIDARG;
    auto* clone = new fake_stream(data_);
    clone->position_ = position_;
    *output = clone;
    return S_OK;
  }

 private:
  std::atomic<ULONG> references_{1};
  std::string data_;
  std::size_t position_ = 0;
};

struct fake_entry {
  std::wstring name;
  std::string bytes;
  DWORD attributes = FILE_ATTRIBUTE_NORMAL;
  bool has_size = true;
};

class fake_data_object final : public IDataObject {
 public:
  bool descriptor = true;
  bool contents = true;
  bool hdrop = true;
  bool drop_was_read = false;
  std::vector<fake_entry> entries;

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
    if (format->cfFormat ==
        static_cast<CLIPFORMAT>(molink::file_descriptor_format())) {
      if (!descriptor) return DV_E_FORMATETC;
      return descriptor_medium(medium);
    }
    if (format->cfFormat ==
        static_cast<CLIPFORMAT>(molink::file_contents_format())) {
      if (!contents) return DV_E_FORMATETC;
      if (format->lindex < 0 ||
          static_cast<std::size_t>(format->lindex) >= entries.size()) {
        return DV_E_LINDEX;
      }
      medium->tymed = TYMED_ISTREAM;
      medium->pstm = new fake_stream(entries[format->lindex].bytes);
      medium->pUnkForRelease = nullptr;
      return S_OK;
    }
    if (format->cfFormat == CF_HDROP) {
      drop_was_read = true;
      return DV_E_FORMATETC;
    }
    return DV_E_FORMATETC;
  }

  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
    if (format == nullptr) return E_INVALIDARG;
    if (format->cfFormat ==
        static_cast<CLIPFORMAT>(molink::file_descriptor_format())) {
      return descriptor ? S_OK : DV_E_FORMATETC;
    }
    if (format->cfFormat ==
        static_cast<CLIPFORMAT>(molink::file_contents_format())) {
      if (!contents) return DV_E_FORMATETC;
      if (format->lindex < 0 ||
          static_cast<std::size_t>(format->lindex) >= entries.size()) {
        return DV_E_LINDEX;
      }
      return S_OK;
    }
    if (format->cfFormat == CF_HDROP) return hdrop ? S_OK : DV_E_FORMATETC;
    return DV_E_FORMATETC;
  }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*,
                                                  FORMATETC*) override {
    return DATA_S_SAMEFORMATETC;
  }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*,
                                    BOOL) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD,
                                          IEnumFORMATETC**) override {
    return E_NOTIMPL;
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
  HRESULT descriptor_medium(STGMEDIUM* medium) {
    const std::size_t bytes =
        sizeof(UINT) + entries.size() * sizeof(FILEDESCRIPTORW);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (memory == nullptr) return E_OUTOFMEMORY;
    auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(memory));
    if (group == nullptr) {
      GlobalFree(memory);
      return E_OUTOFMEMORY;
    }
    group->cItems = static_cast<UINT>(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
      FILEDESCRIPTORW& entry = group->fgd[i];
      entry.dwFlags = FD_ATTRIBUTES | (entries[i].has_size ? FD_FILESIZE : 0);
      entry.dwFileAttributes = entries[i].attributes;
      entry.nFileSizeLow =
          static_cast<DWORD>(entries[i].bytes.size() & 0xffffffffull);
      std::memcpy(entry.cFileName, entries[i].name.c_str(),
                  entries[i].name.size() * sizeof(wchar_t));
    }
    GlobalUnlock(memory);
    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = memory;
    medium->pUnkForRelease = nullptr;
    return S_OK;
  }

  std::atomic<ULONG> references_{1};
};

fake_data_object* make_object() {
  auto* object = new fake_data_object();
  object->entries = {
      {L"folder", "", FILE_ATTRIBUTE_DIRECTORY, true},
      {L"first.bin", "0123456789", FILE_ATTRIBUTE_NORMAL, true},
      {L"second.txt", "abcdef", FILE_ATTRIBUTE_NORMAL, true},
  };
  return object;
}

void test_descriptor_preferred() {
  fake_data_object* object = make_object();
  std::vector<molink::data_object_file> files;
  expect(molink::describe_data_object(object, files),
         "descriptor and contents are recognised");
  expect(files.size() == 2, "the directory entry is skipped");
  if (files.size() == 2) {
    expect(files[0].name == "first.bin", "the first name is decoded");
    expect(files[0].size == 10, "the first size is decoded");
    expect(files[1].name == "second.txt", "the second name is decoded");
    // The directory was index 0, so the remaining streams keep lindex 1 and 2
    // rather than shifting down with the filtered vector.
    expect(files[0].lindex == 1, "the filtered lindex is preserved");
    expect(files[1].lindex == 2, "the second lindex is preserved");
  }
  expect(!object->drop_was_read, "CF_HDROP is not consulted");
  object->Release();
}

void test_no_contents_falls_back() {
  fake_data_object* object = make_object();
  object->contents = false;
  std::vector<molink::data_object_file> files;
  expect(!molink::describe_data_object(object, files),
         "descriptor without FileContents is rejected");
  expect(files.empty(), "no entries survive the rejection");
  expect(!object->drop_was_read, "the fallback decision stays with the caller");
  object->Release();
}

void test_no_descriptor_falls_back() {
  fake_data_object* object = make_object();
  object->descriptor = false;
  std::vector<molink::data_object_file> files;
  expect(!molink::describe_data_object(object, files),
         "missing FileGroupDescriptorW is rejected");
  object->Release();
}

void test_range_reads() {
  fake_data_object* object = make_object();
  std::vector<uint8_t> data;
  expect(molink::read_data_object_range(object, 1, 0, 4, data) == 0,
         "a prefix reads");
  expect(std::string(data.begin(), data.end()) == "0123",
         "the prefix bytes match");

  data.clear();
  expect(molink::read_data_object_range(object, 1, 6, 4, data) == 0,
         "a later range reads");
  expect(std::string(data.begin(), data.end()) == "6789",
         "the later range bytes match");

  data.clear();
  expect(molink::read_data_object_range(object, 1, 10, 0, data) == 0,
         "an empty range at EOF is a success");
  expect(data.empty(), "an empty range returns no bytes");

  object->Release();
}

void test_short_and_out_of_bounds() {
  fake_data_object* object = make_object();
  std::vector<uint8_t> data;
  // The descriptor claims 10 bytes; ask for 12 to force a short read past the
  // stream's real end. A short answer must not be padded or partially
  // accepted: the viewer declared the size.
  expect(molink::read_data_object_range(object, 1, 0, 12, data) == 1,
         "a short read is a read error");
  expect(data.empty(), "a failed read returns no bytes");

  data.clear();
  expect(molink::read_data_object_range(object, 1, 99, 4, data) == 1,
         "an offset past EOF is a read error");
  expect(data.empty(), "an out-of-range read returns no bytes");

  data.clear();
  expect(molink::read_data_object_range(object, 7, 0, 4, data) == 1,
         "an unknown lindex is a read error");
  object->Release();
}

void test_tokens() {
  uint32_t generation = 0;
  uint32_t index = 0;
  const uint64_t token = molink::clipboard_stream_token(7, 3);
  expect(token != 0, "a token is never zero");
  expect(molink::clipboard_stream_token_split(token, generation, index),
         "a token splits");
  expect(generation == 7 && index == 3, "a token round-trips");
  expect(!molink::clipboard_stream_token_split(0, generation, index),
         "zero is not a token");
  expect(!molink::clipboard_stream_token_split(
             molink::clipboard_stream_token(0, 0), generation, index),
         "generation zero is not a token");
  expect(molink::clipboard_stream_token(7, 3) !=
             molink::clipboard_stream_token(8, 3),
         "a new generation changes the token");
}

}  // namespace

int main() {
  const HRESULT ole = OleInitialize(nullptr);
  if (FAILED(ole)) {
    std::printf("OleInitialize failed: %08lx\n",
                static_cast<unsigned long>(ole));
    return 1;
  }
  test_descriptor_preferred();
  test_no_contents_falls_back();
  test_no_descriptor_falls_back();
  test_range_reads();
  test_short_and_out_of_bounds();
  test_tokens();
  OleUninitialize();
  std::printf("%d checks, %d failure(s)\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
