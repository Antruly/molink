#include "clipboard/windows_data_object.h"

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
#include <cstddef>
#include <cstring>
#include <memory>

namespace molink {
namespace {

std::string wide_to_utf8(const wchar_t* text, std::size_t max_length) {
  if (text == nullptr) return std::string();
  std::size_t length = 0;
  while (length < max_length && text[length] != L'\0') ++length;
  if (length == 0) return std::string();
  const int needed = WideCharToMultiByte(CP_UTF8, 0, text,
                                         static_cast<int>(length), nullptr, 0,
                                         nullptr, nullptr);
  if (needed <= 0) return std::string();
  std::string out(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), out.data(),
                      needed, nullptr, nullptr);
  return out;
}

/// The descriptor is a variable-length array; GlobalSize bounds a malformed
/// owner that claims more items than it allocated.
bool descriptor_size_ok(HGLOBAL memory, UINT count) {
  if (count == 0) return false;
  const SIZE_T bytes = GlobalSize(memory);
  if (bytes < sizeof(UINT)) return false;
  // A hostile cItems would overflow the multiply; 4096 is far beyond the
  // 8-file clipboard contract and keeps the arithmetic safe.
  if (count > 4096) return false;
  const std::size_t needed =
      sizeof(UINT) + static_cast<std::size_t>(count) * sizeof(FILEDESCRIPTORW);
  return bytes >= needed;
}

}  // namespace

UINT file_descriptor_format() {
  static const UINT format =
      RegisterClipboardFormatW(L"FileGroupDescriptorW");
  return format;
}

UINT file_contents_format() {
  static const UINT format = RegisterClipboardFormatW(L"FileContents");
  return format;
}

bool describe_data_object(IDataObject* object,
                          std::vector<data_object_file>& files) {
  files.clear();
  if (object == nullptr) return false;

  FORMATETC format = {};
  format.cfFormat = static_cast<CLIPFORMAT>(file_descriptor_format());
  format.dwAspect = DVASPECT_CONTENT;
  format.lindex = -1;
  format.tymed = TYMED_HGLOBAL;

  STGMEDIUM medium = {};
  if (FAILED(object->GetData(&format, &medium))) return false;
  if (medium.tymed != TYMED_HGLOBAL || medium.hGlobal == nullptr) {
    ReleaseStgMedium(&medium);
    return false;
  }

  bool found = false;
  auto* group =
      static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(medium.hGlobal));
  if (group != nullptr) {
    const UINT count = group->cItems;
    if (descriptor_size_ok(medium.hGlobal, count)) {
      for (UINT i = 0; i < count; ++i) {
        const FILEDESCRIPTORW& entry = group->fgd[i];
        // CF_HDROP can carry folders; a folder has no bytes to pull. Keep the
        // original descriptor index because FileContents uses it.
        if ((entry.dwFlags & FD_ATTRIBUTES) != 0 &&
            (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
          continue;
        }
        data_object_file file;
        file.name = wide_to_utf8(entry.cFileName, MAX_PATH);
        if (file.name.empty()) continue;
        if ((entry.dwFlags & FD_FILESIZE) != 0) {
          file.size = (static_cast<uint64_t>(entry.nFileSizeHigh) << 32) |
                      static_cast<uint64_t>(entry.nFileSizeLow);
        }
        file.lindex = static_cast<long>(i);
        files.push_back(std::move(file));
      }
      found = !files.empty();
    }
    GlobalUnlock(medium.hGlobal);
  }
  ReleaseStgMedium(&medium);
  if (!found) {
    files.clear();
    return false;
  }

  // Descriptor without FileContents is a real-file offer (Explorer, some
  // shell extensions). It must use the CF_HDROP path, not a stream that the
  // owner will refuse.
  FORMATETC contents = {};
  contents.cfFormat = static_cast<CLIPFORMAT>(file_contents_format());
  contents.dwAspect = DVASPECT_CONTENT;
  contents.lindex = files.front().lindex;
  contents.tymed = TYMED_ISTREAM;
  if (FAILED(object->QueryGetData(&contents))) {
    files.clear();
    return false;
  }
  return true;
}

bool open_data_object_stream(IDataObject* object, long lindex,
                             data_object_stream& out) {
  if (object == nullptr || lindex < 0) return false;
  FORMATETC format = {};
  format.cfFormat = static_cast<CLIPFORMAT>(file_contents_format());
  format.dwAspect = DVASPECT_CONTENT;
  format.lindex = lindex;
  format.tymed = TYMED_ISTREAM;

  STGMEDIUM medium = {};
  if (FAILED(object->GetData(&format, &medium))) return false;
  if (medium.tymed != TYMED_ISTREAM || medium.pstm == nullptr) {
    ReleaseStgMedium(&medium);
    return false;
  }
  out.medium = medium;
  out.stream = medium.pstm;
  return true;
}

uint32_t read_data_object_stream(IStream* stream, uint64_t offset,
                                 uint64_t length, std::vector<uint8_t>& data) {
  if (stream == nullptr) return 1;
  if (length == 0) return 0;
  // One pull is at most 256 KiB, but keep the helper safe for a larger
  // caller by reading in bounded slices.
  constexpr std::size_t kSlice = 1024 * 1024;
  std::vector<uint8_t> buffer(static_cast<std::size_t>(length));
  LARGE_INTEGER position = {};
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (FAILED(stream->Seek(position, STREAM_SEEK_SET, nullptr))) {
    return 1;
  }
  std::size_t total = 0;
  while (total < buffer.size()) {
    const ULONG want = static_cast<ULONG>(
        std::min<std::size_t>(buffer.size() - total, kSlice));
    ULONG got = 0;
    const HRESULT result = stream->Read(buffer.data() + total, want, &got);
    if (FAILED(result)) return 1;
    if (got == 0) break;
    total += got;
  }
  if (total != buffer.size()) {
    // mstsc ended the stream before the size it advertised. The viewer already
    // declared that size to its staging layer; a short answer is a failure,
    // not end of file.
    return 1;
  }
  data.insert(data.end(), buffer.begin(), buffer.end());
  return 0;
}

uint32_t read_data_object_range(IDataObject* object, long lindex,
                                uint64_t offset, uint64_t length,
                                std::vector<uint8_t>& data) {
  data_object_stream stream;
  if (!open_data_object_stream(object, lindex, stream)) return 1;
  return read_data_object_stream(stream.stream, offset, length, data);
}

uint64_t clipboard_stream_token(uint32_t generation, uint32_t index) {
  // index + 1 keeps 0 reserved for "real path"; generation 0 is skipped by
  // the caller so a token always names a generation.
  return (static_cast<uint64_t>(generation) << 32) |
         (static_cast<uint64_t>(index) + 1);
}

bool clipboard_stream_token_split(uint64_t token, uint32_t& generation,
                                  uint32_t& index) {
  if (token == 0) return false;
  const uint32_t encoded = static_cast<uint32_t>(token & 0xffffffffull);
  if (encoded == 0) return false;
  const uint32_t generation_value = static_cast<uint32_t>(token >> 32);
  if (generation_value == 0) return false;
  generation = generation_value;
  index = encoded - 1;
  return true;
}

}  // namespace molink

#endif  // _WIN32
