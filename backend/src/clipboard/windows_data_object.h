#pragma once

#if defined(_WIN32)

#include <objidl.h>

#include <cstdint>
#include <string>
#include <vector>

namespace molink {

/// One file advertised by the clipboard's OLE data object.
///
/// `lindex` is the FORMATETC index of the FileContents stream and is **not**
/// the position in this vector: directory entries are skipped, so a filtered
/// list would otherwise ask for the wrong stream.
struct data_object_file {
  std::string name;
  uint64_t size = 0;
  long lindex = -1;
};

/// Registered clipboard formats used by the virtual-file (RDP-style) clipboard.
/// `RegisterClipboardFormatW` is idempotent, so both translation units may ask.
UINT file_descriptor_format();
UINT file_contents_format();

/// Enumerates `FileGroupDescriptorW` and checks that `FileContents` is actually
/// available. Returns false when either format is missing or malformed, which
/// tells the caller to fall back to CF_HDROP.
bool describe_data_object(IDataObject* object,
                          std::vector<data_object_file>& files);

/// A FileContents stream plus the STGMEDIUM that owns it. Kept alive for one
/// clipboard generation so a 256 KiB pull does not reopen the RDP stream.
struct data_object_stream {
  STGMEDIUM medium{};
  IStream* stream = nullptr;

  data_object_stream() = default;
  data_object_stream(const data_object_stream&) = delete;
  data_object_stream& operator=(const data_object_stream&) = delete;
  ~data_object_stream() {
    if (stream != nullptr) ReleaseStgMedium(&medium);
  }
};

/// Obtains one file's FileContents stream. False when the index is invalid or
/// the data object refused the medium.
bool open_data_object_stream(IDataObject* object, long lindex,
                             data_object_stream& out);

/// Reads exactly `length` bytes through `stream` at `offset` and appends them
/// to `data`. Returns the wire status: 0 on success, 1 when the stream ended
/// early or the read failed. A short read is never padded: the viewer declared
/// the size, so a short answer must fail rather than truncate the paste.
uint32_t read_data_object_stream(IStream* stream, uint64_t offset,
                                 uint64_t length, std::vector<uint8_t>& data);

/// One-shot helper used by the tests and by callers that do not cache.
uint32_t read_data_object_range(IDataObject* object, long lindex,
                                uint64_t offset, uint64_t length,
                                std::vector<uint8_t>& data);

/// Opaque identity of one entry inside the data-object cache. The high 32 bits
/// are the clipboard generation (a fresh value every time mstsc replaces its
/// data object); the low 32 bits are the public list index. 0 is never a valid
/// token, so a virtual entry is distinguishable from a real CF_HDROP path.
uint64_t clipboard_stream_token(uint32_t generation, uint32_t index);
bool clipboard_stream_token_split(uint64_t token, uint32_t& generation,
                                  uint32_t& index);

}  // namespace molink

#endif  // _WIN32
