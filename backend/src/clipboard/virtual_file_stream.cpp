#include "clipboard/virtual_file_stream.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace molink {

bool virtual_stream_seek(uint64_t size, uint64_t position, int64_t move,
                         virtual_stream_seek_origin origin,
                         uint64_t& new_position) {
  int64_t base = 0;
  switch (origin) {
    case virtual_stream_seek_origin::begin:
      base = 0;
      break;
    case virtual_stream_seek_origin::current:
      base = static_cast<int64_t>(position);
      break;
    case virtual_stream_seek_origin::end:
      base = static_cast<int64_t>(size);
      break;
    default:
      return false;
  }
  if (move > 0 && base > std::numeric_limits<int64_t>::max() - move) {
    return false;
  }
  if (move < 0 && base < std::numeric_limits<int64_t>::min() - move) {
    return false;
  }
  const int64_t target = base + move;
  if (target < 0) return false;
  new_position = static_cast<uint64_t>(target);
  return true;
}

virtual_stream_source::virtual_stream_source(
    std::vector<virtual_mount_file> files, virtual_file_reader* reader)
    : files_(std::move(files)), reader_(reader) {}

void virtual_stream_source::stop() {
  std::lock_guard<std::mutex> lock(mutex_);
  stopped_ = true;
  reader_ = nullptr;
}

bool virtual_stream_source::stopped() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stopped_;
}

std::size_t virtual_stream_source::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return files_.size();
}

const virtual_mount_file& virtual_stream_source::file(std::size_t index) const {
  // The vector is immutable after construction, so the reference stays valid
  // without holding the lock.
  return files_[index];
}

virtual_stream_read_result virtual_stream_source::read(
    std::size_t index, uint64_t offset, void* buffer,
    uint32_t capacity) const {
  virtual_stream_read_result result;
  virtual_file_reader* reader = nullptr;
  uint32_t file_id = 0;
  uint64_t file_size = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || reader_ == nullptr) {
      result.status = virtual_stream_status::stopped;
      return result;
    }
    if (index >= files_.size()) {
      result.status = virtual_stream_status::read_error;
      return result;
    }
    if (buffer == nullptr && capacity != 0) {
      result.status = virtual_stream_status::read_error;
      return result;
    }
    file_id = files_[index].id;
    file_size = files_[index].size;
    reader = reader_;
  }
  if (capacity == 0 || offset >= file_size) {
    return result;  // clean end of file
  }
  const uint64_t want = std::min<uint64_t>(
      {static_cast<uint64_t>(capacity), file_size - offset,
       kVirtualStreamBlockBytes});
  if (want > std::numeric_limits<uint32_t>::max()) {
    result.status = virtual_stream_status::read_error;
    return result;
  }
  int got = -1;
  try {
    got = reader->read_file_range(file_id, offset,
                                  static_cast<uint32_t>(want),
                                  static_cast<char*>(buffer),
                                  static_cast<std::size_t>(want));
  } catch (...) {
    // IStream::Read is called by Explorer's paste thread. A publisher that
    // throws must become a failed copy, never an escaping exception.
    result.status = virtual_stream_status::read_error;
    return result;
  }
  if (got < 0 || static_cast<uint64_t>(got) > want) {
    result.status = virtual_stream_status::read_error;
    return result;
  }
  result.bytes = static_cast<uint32_t>(got);
  return result;
}

virtual_stream_cursor::virtual_stream_cursor(
    std::shared_ptr<virtual_stream_source> source, std::size_t index)
    : source_(std::move(source)), index_(index) {}

uint64_t virtual_stream_cursor::size() const {
  if (!source_ || index_ >= source_->size()) return 0;
  return source_->file(index_).size;
}

uint64_t virtual_stream_cursor::position() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return position_;
}

bool virtual_stream_cursor::seek(int64_t move,
                                 virtual_stream_seek_origin origin,
                                 uint64_t& new_position) {
  if (!source_ || index_ >= source_->size()) return false;
  const uint64_t file_size = source_->file(index_).size;
  std::lock_guard<std::mutex> lock(mutex_);
  uint64_t target = 0;
  if (!virtual_stream_seek(file_size, position_, move, origin, target)) {
    return false;
  }
  position_ = target;
  new_position = target;
  return true;
}

virtual_stream_read_result virtual_stream_cursor::read(void* buffer,
                                                       uint32_t capacity) {
  virtual_stream_read_result result;
  if (!source_ || index_ >= source_->size()) {
    result.status = virtual_stream_status::read_error;
    return result;
  }
  const uint64_t file_size = source_->file(index_).size;
  std::lock_guard<std::mutex> lock(mutex_);
  if (position_ >= file_size) return result;  // clean EOF
  const uint64_t want = std::min<uint64_t>(
      {static_cast<uint64_t>(capacity), file_size - position_,
       kVirtualStreamBlockBytes});
  result = source_->read(index_, position_, buffer,
                         static_cast<uint32_t>(want));
  if (result.status == virtual_stream_status::ok) {
    position_ += result.bytes;
  }
  return result;
}

}  // namespace molink
