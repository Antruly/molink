// Viewer-side virtual-file stream rules, without COM.
//
// These are the byte accounting rules Explorer's paste depends on: one read
// asks for at most 256 KiB, reads stop cleanly at the advertised size, a
// failed range is a failure (not a short successful read), and a stopped offer
// refuses further reads. The same source feeds the Windows OLE data object, so
// this test covers its IStream logic on every platform.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "clipboard/virtual_file_stream.h"

namespace {

using molink::kVirtualStreamBlockBytes;
using molink::virtual_file_reader;
using molink::virtual_mount_file;
using molink::virtual_stream_cursor;
using molink::virtual_stream_read_result;
using molink::virtual_stream_seek_origin;
using molink::virtual_stream_source;
using molink::virtual_stream_status;

int checks = 0;
int failures = 0;

void expect(bool ok, const char* message) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAIL: %s\n", message);
  }
}

/// Deterministic bytes so a wrong offset is visible, not just a wrong length.
std::vector<uint8_t> pattern(uint64_t size) {
  std::vector<uint8_t> data(static_cast<std::size_t>(size));
  for (std::size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<uint8_t>((i * 31 + 7) & 0xff);
  }
  return data;
}

class fake_reader final : public virtual_file_reader {
 public:
  struct call {
    uint32_t id = 0;
    uint64_t offset = 0;
    uint32_t length = 0;
  };

  fake_reader(std::vector<std::vector<uint8_t>> files,
              std::vector<uint32_t> ids)
      : files_(std::move(files)), ids_(std::move(ids)) {}

  int read_file_range(uint32_t file_id, uint64_t offset, uint64_t length,
                      char* buffer, std::size_t capacity) override {
    calls.push_back({file_id, offset, static_cast<uint32_t>(length)});
    if (fail_all) return -5;
    if (fail_at_call >= 0 &&
        calls.size() == static_cast<std::size_t>(fail_at_call)) {
      return -5;
    }
    std::size_t index = files_.size();
    for (std::size_t i = 0; i < ids_.size(); ++i) {
      if (ids_[i] == file_id) {
        index = i;
        break;
      }
    }
    if (index >= files_.size()) return -5;
    if (offset > files_[index].size()) return -5;
    const std::size_t available =
        static_cast<std::size_t>(files_[index].size() - offset);
    const std::size_t take = std::min<std::size_t>(
        {available, static_cast<std::size_t>(length), capacity});
    if (take != 0) {
      std::memcpy(buffer, files_[index].data() + offset, take);
    }
    return static_cast<int>(take);
  }

  std::vector<call> calls;
  bool fail_all = false;
  int fail_at_call = -1;

 private:
  std::vector<std::vector<uint8_t>> files_;
  std::vector<uint32_t> ids_;
};

std::shared_ptr<virtual_stream_source> make_source(
    fake_reader& reader, std::vector<std::vector<uint8_t>> files) {
  std::vector<virtual_mount_file> entries;
  for (std::size_t i = 0; i < files.size(); ++i) {
    virtual_mount_file file;
    file.id = static_cast<uint32_t>(i + 100);
    file.name = "file" + std::to_string(i) + ".bin";
    file.size = files[i].size();
    entries.push_back(file);
  }
  return std::make_shared<virtual_stream_source>(entries, &reader);
}

void range_cap_and_accounting() {
  const std::vector<uint8_t> data = pattern(kVirtualStreamBlockBytes * 3 + 17);
  fake_reader reader({data}, {100});
  auto source = make_source(reader, {data});
  virtual_stream_cursor cursor(source, 0);

  std::vector<uint8_t> buffer(kVirtualStreamBlockBytes * 4);
  const virtual_stream_read_result first =
      cursor.read(buffer.data(), static_cast<uint32_t>(buffer.size()));
  expect(first.status == virtual_stream_status::ok, "first read succeeds");
  expect(first.bytes == kVirtualStreamBlockBytes,
         "one read is capped at 256 KiB");
  expect(cursor.position() == kVirtualStreamBlockBytes,
         "the position advances by the bytes returned");
  expect(std::memcmp(buffer.data(), data.data(), first.bytes) == 0,
         "the first block matches the source");
  expect(reader.calls.size() == 1, "one block is one reader call");
  expect(reader.calls[0].offset == 0, "the first call starts at zero");
  expect(reader.calls[0].length == kVirtualStreamBlockBytes,
         "the reader is asked for exactly one block");
  expect(reader.calls[0].id == 100, "the session-scoped id reaches the reader");

  uint64_t read_total = first.bytes;
  while (cursor.position() < data.size()) {
    const virtual_stream_read_result next =
        cursor.read(buffer.data(), static_cast<uint32_t>(buffer.size()));
    expect(next.status == virtual_stream_status::ok, "later reads succeed");
    read_total += next.bytes;
    if (next.bytes == 0) break;
  }
  expect(read_total == data.size(), "the whole file is read once");
  const virtual_stream_read_result eof = cursor.read(buffer.data(), 1);
  expect(eof.status == virtual_stream_status::ok && eof.bytes == 0,
         "reading at the declared size is a clean EOF");
  expect(cursor.position() == data.size(), "EOF does not move the position");
}

void seek_rules() {
  const std::vector<uint8_t> data = pattern(1000);
  fake_reader reader({data}, {100});
  auto source = make_source(reader, {data});
  virtual_stream_cursor cursor(source, 0);

  uint64_t at = 0;
  expect(cursor.seek(100, virtual_stream_seek_origin::begin, at) && at == 100,
         "seek from the beginning");
  expect(cursor.seek(50, virtual_stream_seek_origin::current, at) && at == 150,
         "seek from the current position");
  expect(cursor.seek(-50, virtual_stream_seek_origin::end, at) && at == 950,
         "seek from the end");
  expect(!cursor.seek(-1, virtual_stream_seek_origin::begin, at),
         "a negative absolute position is refused");
  expect(cursor.seek(5000, virtual_stream_seek_origin::begin, at) &&
             at == 5000,
         "seeking past the end is allowed");

  std::vector<uint8_t> buffer(16);
  const virtual_stream_read_result past = cursor.read(buffer.data(), 16);
  expect(past.status == virtual_stream_status::ok && past.bytes == 0,
         "a read past the end returns zero bytes");
  expect(cursor.seek(0, virtual_stream_seek_origin::begin, at), "rewind");
  const virtual_stream_read_result first = cursor.read(buffer.data(), 16);
  expect(first.bytes == 16 &&
             std::memcmp(buffer.data(), data.data(), 16) == 0,
         "a rewound read returns the first bytes");
}

void failure_propagation() {
  const std::vector<uint8_t> data = pattern(4096);
  fake_reader reader({data}, {100});
  auto source = make_source(reader, {data});
  virtual_stream_cursor cursor(source, 0);
  reader.fail_all = true;
  std::vector<uint8_t> buffer(1024);
  const virtual_stream_read_result failed = cursor.read(buffer.data(), 1024);
  expect(failed.status == virtual_stream_status::read_error,
         "a failing range becomes a read error");
  expect(failed.bytes == 0, "a failed range reports no bytes");
  expect(cursor.position() == 0, "a failed read does not advance");

  reader.fail_all = false;
  source->stop();
  const virtual_stream_read_result stopped = cursor.read(buffer.data(), 1024);
  expect(stopped.status == virtual_stream_status::stopped,
         "a stopped offer refuses reads");
  expect(reader.calls.size() == 1, "stop does not call the reader again");
}

void short_read_is_eof() {
  std::vector<uint8_t> data = pattern(600);
  fake_reader reader({data}, {100});
  // The descriptor claims more than the file actually has, e.g. the remote
  // file shrank after it was copied. A short answer is EOF, not an error.
  std::vector<virtual_mount_file> entries;
  virtual_mount_file file;
  file.id = 100;
  file.name = "shrunk.bin";
  file.size = 4096;
  entries.push_back(file);
  auto source = std::make_shared<virtual_stream_source>(entries, &reader);
  virtual_stream_cursor cursor(source, 0);
  std::vector<uint8_t> buffer(1024);
  const virtual_stream_read_result first = cursor.read(buffer.data(), 1024);
  expect(first.status == virtual_stream_status::ok && first.bytes == 600,
         "a short range is returned as-is");
  expect(cursor.position() == 600, "the position follows the short read");
  const virtual_stream_read_result second = cursor.read(buffer.data(), 1024);
  expect(second.status == virtual_stream_status::ok && second.bytes == 0,
         "the next read sees end of file");
}

void invalid_requests() {
  const std::vector<uint8_t> data = pattern(128);
  fake_reader reader({data}, {100});
  auto source = make_source(reader, {data});
  const virtual_stream_read_result bad_index =
      source->read(9, 0, nullptr, 0);
  expect(bad_index.status == virtual_stream_status::read_error,
         "an out-of-range file index fails");
  const virtual_stream_read_result null_buffer =
      source->read(0, 0, nullptr, 16);
  expect(null_buffer.status == virtual_stream_status::read_error,
         "a null buffer with bytes to read fails");
  const virtual_stream_read_result zero =
      source->read(0, 0, nullptr, 0);
  expect(zero.status == virtual_stream_status::ok && zero.bytes == 0,
         "a zero-length read is a no-op");
}

}  // namespace

int main() {
  range_cap_and_accounting();
  seek_rules();
  failure_propagation();
  short_read_is_eof();
  invalid_requests();
  std::printf("%d checks, %d failure(s)\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
