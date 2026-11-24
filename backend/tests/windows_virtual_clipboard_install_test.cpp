// Windows-only integration test for the virtual-file publisher.
//
// It installs a virtual offer with OleSetClipboard, then reads it back through
// OleGetClipboard exactly as another application's paste would: descriptor
// first, FileContents streams second. This is the part the unit test cannot
// cover -- the STA thread, the message loop and the actual clipboard handoff.
//
// It replaces the developer's clipboard, so it is skipped unless
// MOLINK_CLIPBOARD_WINDOWS_TEST=1. CTest treats return code 77 as a skip.

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
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "clipboard/virtual_file_stream.h"
#include "clipboard/virtual_mount.h"
#include "clipboard/windows_data_object.h"
#include "clipboard/windows_virtual_clipboard.h"

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

class fake_reader final : public molink::virtual_file_reader {
 public:
  explicit fake_reader(std::vector<std::vector<uint8_t>> files)
      : files_(std::move(files)) {}

  int read_file_range(uint32_t file_id, uint64_t offset, uint64_t length,
                      char* buffer, std::size_t capacity) override {
    ++calls;
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

 private:
  std::vector<std::vector<uint8_t>> files_;
};

std::vector<uint8_t> pattern(uint64_t size) {
  std::vector<uint8_t> data(static_cast<std::size_t>(size));
  for (std::size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<uint8_t>((i * 29 + 11) & 0xff);
  }
  return data;
}

void run() {
  const std::vector<uint8_t> first = pattern(1500);
  const std::vector<uint8_t> second = pattern(70 * 1024);
  fake_reader reader({first, second});

  std::vector<molink::virtual_mount_file> files(2);
  files[0].id = 1;
  files[0].name = "install-first.bin";
  files[0].size = first.size();
  files[1].id = 2;
  files[1].name = "install-second.bin";
  files[1].size = second.size();

  wchar_t temp_root[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, temp_root);
  const std::filesystem::path mountpoint =
      std::filesystem::path(temp_root) /
      ("molink-virtual-install-" + std::to_string(GetCurrentProcessId()));
  std::error_code ec;
  std::filesystem::create_directories(mountpoint, ec);

  std::string error;
  molink::virtual_mount_factory factory =
      molink::platform_virtual_mount_factory();
  expect(factory != nullptr, "the platform factory exists");
  std::shared_ptr<molink::virtual_mount> mount;
  if (factory != nullptr) {
    mount = factory(mountpoint.string(), files, &reader, error);
  }
  expect(mount != nullptr && mount->mounted(),
         "the virtual offer owns the clipboard");
  expect(molink::windows_virtual_clipboard_owns_clipboard(),
         "the process reports ownership");

  IDataObject* object = nullptr;
  expect(SUCCEEDED(OleGetClipboard(&object)) && object != nullptr,
         "OleGetClipboard returns our data object");
  if (object != nullptr) {
    // Read the formats the way a paste target does. QueryGetData is skipped
    // deliberately: Wine's clipboard proxy answers DV_E_FORMATETC for any
    // FileContents query even though GetData forwards correctly, and Explorer
    // reads with GetData anyway.
    FORMATETC descriptor_format = {};
    descriptor_format.cfFormat =
        static_cast<CLIPFORMAT>(molink::file_descriptor_format());
    descriptor_format.dwAspect = DVASPECT_CONTENT;
    descriptor_format.lindex = -1;
    descriptor_format.tymed = TYMED_HGLOBAL;
    STGMEDIUM descriptor = {};
    expect(SUCCEEDED(object->GetData(&descriptor_format, &descriptor)),
           "FileGroupDescriptorW is readable");
    if (descriptor.tymed == TYMED_HGLOBAL && descriptor.hGlobal != nullptr) {
      auto* group =
          static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(descriptor.hGlobal));
      expect(group != nullptr && group->cItems == 2,
             "both files are described");
      if (group != nullptr && group->cItems == 2) {
        expect(group->fgd[1].nFileSizeLow == second.size(),
               "the descriptor carries the second file's size");
      }
      if (group != nullptr) GlobalUnlock(descriptor.hGlobal);
      ReleaseStgMedium(&descriptor);
    }

    FORMATETC contents_format = {};
    contents_format.cfFormat =
        static_cast<CLIPFORMAT>(molink::file_contents_format());
    contents_format.dwAspect = DVASPECT_CONTENT;
    contents_format.lindex = 1;
    contents_format.tymed = TYMED_ISTREAM;
    STGMEDIUM medium = {};
    expect(SUCCEEDED(object->GetData(&contents_format, &medium)) &&
               medium.tymed == TYMED_ISTREAM && medium.pstm != nullptr,
           "FileContents returns an IStream");
    if (medium.pstm != nullptr) {
      STATSTG stat = {};
      expect(SUCCEEDED(medium.pstm->Stat(&stat, STATFLAG_NONAME)) &&
                 stat.cbSize.QuadPart == second.size(),
             "Stat reports the advertised size");
      LARGE_INTEGER position = {};
      position.QuadPart = 100;
      expect(SUCCEEDED(medium.pstm->Seek(position, STREAM_SEEK_SET, nullptr)),
             "Seek positions the stream");
      std::vector<uint8_t> bytes(500);
      ULONG got = 0;
      expect(SUCCEEDED(medium.pstm->Read(bytes.data(), 500, &got)) &&
                 got == 500,
             "a FileContents range is served");
      expect(got == 500 &&
                 std::memcmp(bytes.data(), second.data() + 100, 500) == 0,
             "the range matches the advertised file");
      expect(reader.calls > 0, "the range went through the reader");
      ReleaseStgMedium(&medium);
    }
    object->Release();
  }

  if (mount) mount->stop();
  expect(!molink::windows_virtual_clipboard_owns_clipboard(),
         "stopping the offer releases ownership");

  // A later paste must not find our object any more.
  IDataObject* after = nullptr;
  if (SUCCEEDED(OleGetClipboard(&after)) && after != nullptr) {
    FORMATETC stale_format = {};
    stale_format.cfFormat =
        static_cast<CLIPFORMAT>(molink::file_descriptor_format());
    stale_format.dwAspect = DVASPECT_CONTENT;
    stale_format.lindex = -1;
    stale_format.tymed = TYMED_HGLOBAL;
    STGMEDIUM stale = {};
    expect(FAILED(after->GetData(&stale_format, &stale)),
           "the released offer is no longer on the clipboard");
    after->Release();
  }

  std::filesystem::remove_all(mountpoint, ec);
}

}  // namespace

int main() {
  const char* enabled = std::getenv("MOLINK_CLIPBOARD_WINDOWS_TEST");
  if (enabled == nullptr || std::string(enabled) != "1") {
    std::printf("skipped: set MOLINK_CLIPBOARD_WINDOWS_TEST=1\n");
    return kSkipped;
  }
  const HRESULT ole = OleInitialize(nullptr);
  run();
  if (SUCCEEDED(ole)) OleUninitialize();
  std::printf("%d checks, %d failure(s)\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
