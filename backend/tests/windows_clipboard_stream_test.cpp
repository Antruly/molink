// Windows-only integration test for the virtual-file (RDP-style) clipboard.
//
// It installs a streaming offer, takes the OLE clipboard, then reads it back
// through IDataObject exactly as Explorer's paste does: FileGroupDescriptorW
// for the list, FileContents (TYMED_ISTREAM) for the bytes, Seek/Stat/Clone on
// the returned IStream, and CF_HDROP absent.
//
// Running it replaces the developer's clipboard, so it is skipped unless
// MOLINK_CLIPBOARD_WINDOWS_TEST=1. CTest treats return code 77 as a skip.
//
// The source is a fake: it answers every range synchronously, which proves the
// data object and IStream layer. The network side of the same path (the 10 s /
// 60 s fetch budgets, request-id matching, session loss) is covered on every
// platform by clipboard_lazy_test.cpp.

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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "clipboard/clipboard.h"
#include "clipboard_lazy.h"

namespace {

constexpr int kSkipped = 77;

int checks = 0;
int failures = 0;

void expect(bool ok, const char* message) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAIL: %s\n", message);
  }
}

class fake_source final : public molink::lazy_blob_source {
 public:
  fake_source(std::vector<std::string> names, std::vector<std::string> data)
      : data_(std::move(data)) {
    for (std::size_t i = 0; i < names.size(); ++i) {
      molink::lazy_clipboard_file file;
      file.name = names[i];
      file.size = data_[i].size();
      files_.push_back(std::move(file));
    }
  }

  const std::vector<molink::lazy_clipboard_file>& files() const override {
    return files_;
  }
  std::size_t size() const override { return files_.size(); }
  bool streaming() const override { return true; }

  bool read_range(std::size_t index, uint64_t offset, uint32_t length,
                  std::vector<uint8_t>& out, std::string& error,
                  uint64_t, uint64_t) override {
    ++requests;
    if (index >= data_.size() || offset > data_[index].size()) {
      error = "bad range";
      return false;
    }
    const std::size_t available =
        data_[index].size() - static_cast<std::size_t>(offset);
    const std::size_t take =
        available < length ? available : static_cast<std::size_t>(length);
    out.assign(data_[index].begin() + static_cast<std::ptrdiff_t>(offset),
               data_[index].begin() +
                   static_cast<std::ptrdiff_t>(offset + take));
    return true;
  }

  void fail_all(const std::string&) override { closed_ = true; }
  bool closed() const override { return closed_; }

  int requests = 0;

 private:
  bool closed_ = false;
  std::vector<molink::lazy_clipboard_file> files_;
  std::vector<std::string> data_;
};

std::string read_stream(IStream* stream, ULONG chunk) {
  std::string out;
  std::vector<char> buffer(chunk);
  for (;;) {
    ULONG got = 0;
    const HRESULT result = stream->Read(buffer.data(), chunk, &got);
    if (FAILED(result)) {
      expect(false, "IStream::Read failed");
      break;
    }
    if (got == 0) break;
    out.append(buffer.data(), got);
  }
  return out;
}

}  // namespace

int main() {
  if (std::getenv("MOLINK_CLIPBOARD_WINDOWS_TEST") == nullptr) {
    std::printf("skipped: set MOLINK_CLIPBOARD_WINDOWS_TEST=1 to run\n");
    return kSkipped;
  }
  const HRESULT ole = OleInitialize(nullptr);
  if (FAILED(ole)) {
    std::printf("skipped: OleInitialize failed %08lx\n",
                static_cast<unsigned long>(ole));
    return kSkipped;
  }

  const std::string first(200000, 'A');
  std::string second;
  for (int i = 0; i < 1000; ++i) {
    second.push_back(static_cast<char>('a' + i % 26));
  }

  auto source = std::make_shared<fake_source>(
      std::vector<std::string>{"first.bin", "second.txt"},
      std::vector<std::string>{first, second});
  auto registry = std::make_shared<molink::lazy_clipboard>();
  expect(registry->offer(42, "~/moLink", source->files(), source) ==
             molink::lazy_offer_error::none,
         "offer registered");

  std::string error;
  auto clipboard = molink::make_windows_clipboard(error);
  expect(clipboard != nullptr, "windows clipboard backend exists");
  if (!clipboard) return 1;
  if (!clipboard->install_lazy(registry, error)) {
    std::printf("skipped: install_lazy failed: %s\n", error.c_str());
    return kSkipped;
  }

  IDataObject* object = nullptr;
  expect(SUCCEEDED(OleGetClipboard(&object)) && object != nullptr,
         "OleGetClipboard returned an object");
  if (!object) {
    clipboard->uninstall_lazy();
    return 1;
  }

  FORMATETC descriptor_format = {
      static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptorW")),
      nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM descriptor{};
  expect(SUCCEEDED(object->GetData(&descriptor_format, &descriptor)),
         "GetData(FileGroupDescriptorW) succeeded");
  if (descriptor.hGlobal != nullptr) {
    auto* group =
        static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(descriptor.hGlobal));
    expect(group != nullptr && group->cItems == 2, "two descriptors");
    if (group != nullptr) {
      expect(group->fgd[0].nFileSizeLow == first.size(),
             "first file size matches");
      expect(std::wcscmp(group->fgd[0].cFileName, L"first.bin") == 0,
             "first file name matches");
      expect(std::wcscmp(group->fgd[1].cFileName, L"second.txt") == 0,
             "second file name matches");
    }
    GlobalUnlock(descriptor.hGlobal);
    ReleaseStgMedium(&descriptor);
  }

  FORMATETC drop_format = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1,
                           TYMED_HGLOBAL};
  expect(FAILED(object->QueryGetData(&drop_format)),
         "CF_HDROP is not offered");
  STGMEDIUM drop{};
  expect(FAILED(object->GetData(&drop_format, &drop)),
         "GetData(CF_HDROP) is refused");

  FORMATETC contents_format = {
      static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileContents")),
      nullptr, DVASPECT_CONTENT, 0, TYMED_ISTREAM};
  STGMEDIUM contents{};
  expect(SUCCEEDED(object->GetData(&contents_format, &contents)) &&
             contents.pstm != nullptr,
         "GetData(FileContents, lindex 0) returned a stream");
  if (contents.pstm != nullptr) {
    IStream* stream = contents.pstm;
    expect(read_stream(stream, 64 * 1024) == first,
           "first file bytes match");
    STATSTG stat{};
    expect(SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)),
           "Stat succeeded");
    expect(stat.cbSize.QuadPart == first.size(), "Stat size matches");

    LARGE_INTEGER zero{};
    ULARGE_INTEGER position{};
    expect(SUCCEEDED(stream->Seek(zero, STREAM_SEEK_SET, &position)) &&
               position.QuadPart == 0,
           "Seek(0) succeeded");
    expect(read_stream(stream, 4096) == first,
           "reread after seek matches");

    LARGE_INTEGER beyond{};
    beyond.QuadPart = 10;
    expect(SUCCEEDED(stream->Seek(beyond, STREAM_SEEK_END, &position)) &&
               position.QuadPart ==
                   static_cast<ULONGLONG>(beyond.QuadPart) + first.size(),
           "Seek past the end is allowed");
    ULONG tail = 1;
    expect(SUCCEEDED(stream->Read(&tail, 1, &tail)) && tail == 0,
           "Read past the end returns zero bytes");

    expect(SUCCEEDED(stream->Seek(zero, STREAM_SEEK_SET, &position)),
           "rewind for clone");
    IStream* clone = nullptr;
    expect(SUCCEEDED(stream->Clone(&clone)) && clone != nullptr,
           "Clone succeeded");
    if (clone != nullptr) {
      expect(read_stream(clone, 8192) == first,
             "the clone reads the same bytes");
      clone->Release();
    }
    ReleaseStgMedium(&contents);
  }

  contents_format.lindex = 1;
  STGMEDIUM second_medium{};
  expect(SUCCEEDED(object->GetData(&contents_format, &second_medium)) &&
             second_medium.pstm != nullptr,
         "GetData(FileContents, lindex 1) returned a stream");
  if (second_medium.pstm != nullptr) {
    expect(read_stream(second_medium.pstm, 256) == second,
           "second file bytes match");
    ReleaseStgMedium(&second_medium);
  }

  expect(source->requests > 0, "the bytes came through read_range");

  object->Release();
  clipboard->uninstall_lazy();
  registry->clear_all("test done");
  OleUninitialize();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
