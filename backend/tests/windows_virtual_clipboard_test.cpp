// Windows-only unit test for the viewer-side virtual-file data object.
//
// It builds the same IDataObject that gets published with OleSetClipboard and
// reads it the way Explorer's paste does: FileGroupDescriptorW for the list,
// FileContents (TYMED_ISTREAM) for the bytes, Stat/Seek/Read on the returned
// stream. Nothing touches the real clipboard, so unlike
// windows_clipboard_stream_test this runs unconditionally on Windows.

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
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "clipboard/virtual_file_stream.h"
#include "clipboard/windows_data_object.h"
#include "clipboard/windows_virtual_clipboard.h"

namespace {

using molink::virtual_file_reader;
using molink::virtual_mount_file;
using molink::virtual_stream_source;

int checks = 0;
int failures = 0;

void expect(bool ok, const char* message) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAIL: %s\n", message);
  }
}

class fake_reader final : public virtual_file_reader {
 public:
  explicit fake_reader(std::vector<std::vector<uint8_t>> files)
      : files_(std::move(files)) {}

  int read_file_range(uint32_t file_id, uint64_t offset, uint64_t length,
                      char* buffer, std::size_t capacity) override {
    ++calls;
    if (fail) return -5;
    const std::size_t index = file_id - 1;
    if (index >= files_.size() || offset > files_[index].size()) return -5;
    const std::size_t available =
        static_cast<std::size_t>(files_[index].size() - offset);
    const std::size_t take = std::min<std::size_t>(
        {available, static_cast<std::size_t>(length), capacity});
    if (take != 0) std::memcpy(buffer, files_[index].data() + offset, take);
    return static_cast<int>(take);
  }

  int calls = 0;
  bool fail = false;

 private:
  std::vector<std::vector<uint8_t>> files_;
};

std::vector<uint8_t> pattern(uint64_t size) {
  std::vector<uint8_t> data(static_cast<std::size_t>(size));
  for (std::size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<uint8_t>((i * 17 + 3) & 0xff);
  }
  return data;
}

std::wstring to_wide(const std::string& text) {
  const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr,
                                         0);
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      wide.data(), needed);
  return wide;
}

void run() {
  const std::vector<uint8_t> first = pattern(700);
  const std::vector<uint8_t> second = pattern(9 * 1024);
  fake_reader reader({first, second});

  std::vector<virtual_mount_file> files(2);
  files[0].id = 1;
  files[0].name = "first.bin";
  files[0].size = first.size();
  files[1].id = 2;
  files[1].name = "second.bin";
  files[1].size = second.size();

  auto source = std::make_shared<virtual_stream_source>(files, &reader);
  IDataObject* object = nullptr;
  expect(SUCCEEDED(
             molink::create_virtual_file_data_object(source, &object)) &&
             object != nullptr,
         "the data object is created");

  FORMATETC descriptor_format = {};
  descriptor_format.cfFormat =
      static_cast<CLIPFORMAT>(molink::file_descriptor_format());
  descriptor_format.dwAspect = DVASPECT_CONTENT;
  descriptor_format.lindex = -1;
  descriptor_format.tymed = TYMED_HGLOBAL;
  expect(SUCCEEDED(object->QueryGetData(&descriptor_format)),
         "FileGroupDescriptorW is offered");

  FORMATETC contents_format = {};
  contents_format.cfFormat =
      static_cast<CLIPFORMAT>(molink::file_contents_format());
  contents_format.dwAspect = DVASPECT_CONTENT;
  contents_format.lindex = 1;
  contents_format.tymed = TYMED_ISTREAM;
  expect(SUCCEEDED(object->QueryGetData(&contents_format)),
         "FileContents is offered for the second index");

  FORMATETC hdrop_format = {};
  hdrop_format.cfFormat = CF_HDROP;
  hdrop_format.dwAspect = DVASPECT_CONTENT;
  hdrop_format.lindex = -1;
  hdrop_format.tymed = TYMED_HGLOBAL;
  expect(FAILED(object->QueryGetData(&hdrop_format)),
         "CF_HDROP is deliberately absent");

  FORMATETC eager_format = contents_format;
  eager_format.tymed = TYMED_HGLOBAL;
  expect(FAILED(object->QueryGetData(&eager_format)),
         "FileContents is not materialised as HGLOBAL");

  STGMEDIUM descriptor = {};
  expect(SUCCEEDED(object->GetData(&descriptor_format, &descriptor)) &&
             descriptor.tymed == TYMED_HGLOBAL,
         "the descriptor medium is an HGLOBAL");
  auto* group =
      static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(descriptor.hGlobal));
  expect(group != nullptr && group->cItems == 2,
         "the descriptor names both files");
  if (group != nullptr && group->cItems == 2) {
    expect(to_wide("first.bin") == group->fgd[0].cFileName,
           "the first name survives");
    expect(to_wide("second.bin") == group->fgd[1].cFileName,
           "the second name survives");
    expect(group->fgd[1].nFileSizeLow == second.size(),
           "the descriptor carries the file size");
  }
  if (group != nullptr) GlobalUnlock(descriptor.hGlobal);
  ReleaseStgMedium(&descriptor);

  STGMEDIUM medium = {};
  expect(SUCCEEDED(object->GetData(&contents_format, &medium)) &&
             medium.tymed == TYMED_ISTREAM && medium.pstm != nullptr,
         "a FileContents stream is returned");
  IStream* stream = medium.pstm;
  STATSTG stat = {};
  expect(SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) &&
             stat.cbSize.QuadPart == second.size(),
         "Stat reports the advertised size");

  std::vector<uint8_t> buffer(static_cast<std::size_t>(second.size()));
  ULONG total = 0;
  while (total < buffer.size()) {
    ULONG got = 0;
    expect(SUCCEEDED(stream->Read(buffer.data() + total,
                                  static_cast<ULONG>(buffer.size()) - total,
                                  &got)),
           "a stream read succeeds");
    if (got == 0) break;
    total += got;
  }
  expect(total == second.size(), "the whole file is read");
  expect(std::memcmp(buffer.data(), second.data(), second.size()) == 0,
         "the stream returns the advertised bytes");

  LARGE_INTEGER zero = {};
  ULARGE_INTEGER position = {};
  expect(SUCCEEDED(stream->Seek(zero, STREAM_SEEK_SET, &position)) &&
             position.QuadPart == 0,
         "Seek rewinds the stream");
  ULONG got = 0;
  expect(SUCCEEDED(stream->Read(buffer.data(), 16, &got)) && got == 16 &&
             std::memcmp(buffer.data(), second.data(), 16) == 0,
         "a rewound read returns the first bytes");
  ReleaseStgMedium(&medium);

  // A range failure must reach Explorer as an error, not as a truncated file.
  reader.fail = true;
  STGMEDIUM failing = {};
  expect(SUCCEEDED(object->GetData(&contents_format, &failing)),
         "a failing source still yields a stream");
  ULONG failed = 0;
  expect(FAILED(failing.pstm->Read(buffer.data(), 16, &failed)) &&
             failed == 0,
         "a failed range is STG_E_READFAULT");
  ReleaseStgMedium(&failing);

  object->Release();
}

}  // namespace

int main() {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  run();
  if (SUCCEEDED(com)) CoUninitialize();
  std::printf("%d checks, %d failure(s)\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
