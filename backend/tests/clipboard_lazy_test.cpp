#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "clipboard_lazy.h"

namespace {

using molink::lazy_blob_seek;
using molink::lazy_blob_source;
using molink::lazy_clipboard;
using molink::lazy_clipboard_file;
using molink::lazy_file_source;
using molink::lazy_offer_error;
using molink::lazy_seek_origin;
using molink::read_lazy_blob;
using molink::remote_file_source;
using molink::validate_lazy_offer;

int checks = 0;
int failures = 0;

void expect(bool value, const char* message) {
  ++checks;
  if (!value) {
    ++failures;
    std::printf("FAIL: %s\n", message);
  }
}

std::vector<lazy_clipboard_file> make_files(std::size_t count,
                                            uint64_t each = 1) {
  std::vector<lazy_clipboard_file> files;
  for (std::size_t i = 0; i < count; ++i) {
    lazy_clipboard_file file;
    file.name = "file-" + std::to_string(i) + ".bin";
    file.size = each;
    files.push_back(std::move(file));
  }
  return files;
}

std::shared_ptr<lazy_file_source> make_source(
    std::vector<lazy_clipboard_file> files,
    lazy_file_source::request_fn request = {}) {
  return std::make_shared<lazy_file_source>(std::move(files),
                                            std::move(request));
}

void test_offer_limits() {
  expect(validate_lazy_offer(make_files(0)) == lazy_offer_error::bad_file,
         "an empty offer is rejected");
  expect(validate_lazy_offer(make_files(8)) == lazy_offer_error::none,
         "eight files are accepted");
  expect(validate_lazy_offer(make_files(9)) ==
             lazy_offer_error::too_many_files,
         "a ninth file is rejected");
  expect(validate_lazy_offer(
             make_files(2, molink::kMaxLazyBytes / 2)) ==
             lazy_offer_error::none,
         "exactly 2 GiB is accepted");
  expect(validate_lazy_offer(
             make_files(2, molink::kMaxLazyBytes / 2 + 1)) ==
             lazy_offer_error::too_large,
         "one byte over 2 GiB is rejected");

  auto bad = make_files(1);
  bad[0].name = "../escape.txt";
  expect(validate_lazy_offer(bad) == lazy_offer_error::bad_file,
         "a traversal name is rejected");
  bad[0].name = "a\\b";
  expect(validate_lazy_offer(bad) == lazy_offer_error::bad_file,
         "a backslash is rejected");
  bad[0].name = "a/b";
  expect(validate_lazy_offer(bad) == lazy_offer_error::bad_file,
         "a slash is rejected");
  bad[0].name = "";
  expect(validate_lazy_offer(bad) == lazy_offer_error::bad_file,
         "an empty name is rejected");
}

void test_registry_lifecycle() {
  lazy_clipboard registry;
  auto first = make_source(make_files(1));
  expect(registry.offer(7, "/tmp/a", make_files(1), first) ==
             lazy_offer_error::none,
         "a valid offer is registered");
  expect(registry.active(), "the registry is active");
  expect(registry.active_for(7), "the owning session matches");
  expect(!registry.active_for(8), "another session does not match");
  expect(registry.session_id() == 7, "the session id is reported");

  expect(!registry.clear_session(8), "another session cannot clear the offer");
  expect(registry.active(), "the offer survives a foreign clear");
  expect(registry.clear_session(7), "the owner clears the offer");
  expect(!registry.active(), "the registry is empty after clearing");
  expect(first->closed(), "clearing fails the offer's source");

  // Replacing an offer fails the previous source so a blocked render returns
  // instead of waiting out its timeout.
  auto replacement = make_source(make_files(1));
  expect(registry.offer(7, "/tmp/a", make_files(1), replacement) ==
             lazy_offer_error::none,
         "the replacement is registered");
  auto newer = make_source(make_files(2));
  expect(registry.offer(9, "/tmp/b", make_files(2), newer) ==
             lazy_offer_error::none,
         "a newer offer replaces the old one");
  expect(replacement->closed(), "the replaced source is failed");
  expect(registry.active_for(9), "the newer session owns the registry");
}

void test_source_completion() {
  auto source = make_source(make_files(1), [](std::size_t) {});
  std::string path;
  bool ensured = false;
  std::thread waiter([&] {
    ensured = source->ensure(0, path, 5000, 5000);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  source->note_progress(0, 4);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  source->note_done(0, "C:\\inbox\\file-0.bin");
  waiter.join();
  expect(ensured, "a completed file satisfies ensure()");
  expect(path == "C:\\inbox\\file-0.bin", "the rendered path is returned");
}

void test_total_timeout() {
  auto source = make_source(make_files(1), [](std::size_t) {});
  const auto started = std::chrono::steady_clock::now();
  std::string path;
  // The block budget is wider than the whole-file budget, so the total one
  // must be what fires.
  const bool ensured = source->ensure(0, path, 80, 500);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  expect(!ensured, "a file nobody sends times out");
  expect(elapsed >= std::chrono::milliseconds(60),
         "the total budget was actually waited");
  expect(elapsed < std::chrono::seconds(2), "the timeout is bounded");
}

void test_chunk_timeout() {
  auto source = make_source(make_files(1), [](std::size_t) {});
  const auto started = std::chrono::steady_clock::now();
  std::string path;
  // One early progress resets the block deadline once; then the 40 ms block
  // budget expires while the 5 s total budget is still open.
  std::thread progress([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    source->note_progress(0, 1);
  });
  const bool ensured = source->ensure(0, path, 5000, 60);
  progress.join();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  expect(!ensured, "a stalled block fails before the total budget");
  expect(elapsed < std::chrono::seconds(1),
         "the per-block budget is the one that fired");
}

void test_session_loss_wakes_waiter() {
  lazy_clipboard registry;
  auto source = make_source(make_files(1), [](std::size_t) {});
  expect(registry.offer(3, "/tmp", make_files(1), source) ==
             lazy_offer_error::none,
         "the offer is registered before the disconnect");
  bool ensured = true;
  std::string path;
  std::thread waiter([&] {
    ensured = source->ensure(0, path, 5000, 5000);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const auto started = std::chrono::steady_clock::now();
  expect(registry.clear_session(3), "the disconnect clears its offer");
  waiter.join();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  expect(!ensured, "a render waiting on a gone session fails");
  expect(elapsed < std::chrono::milliseconds(500),
         "the waiter is woken immediately, not at the timeout");
}

void test_ensure_all_shares_deadline() {
  auto source = make_source(make_files(3), [](std::size_t) {});
  std::thread completer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    source->note_done(0, "a");
    source->note_done(1, "b");
    source->note_done(2, "c");
  });
  std::vector<std::string> paths;
  const bool ensured = source->ensure_all(paths, 2000, 2000);
  completer.join();
  expect(ensured, "ensure_all waits for every file");
  expect(paths.size() == 3 && paths[0] == "a" && paths[2] == "c",
         "ensure_all returns the paths in offer order");
}

/// A streaming source with deterministic bytes, so the range reader's block
/// boundaries can be checked without a network or a COM stream.
class fake_blob_source final : public lazy_blob_source {
 public:
  explicit fake_blob_source(uint64_t file_size) : files_(1) {
    files_[0].name = "fake.bin";
    files_[0].size = file_size;
  }

  const std::vector<lazy_clipboard_file>& files() const override {
    return files_;
  }
  std::size_t size() const override { return files_.size(); }
  bool streaming() const override { return true; }

  bool read_range(std::size_t index, uint64_t offset, uint32_t length,
                  std::vector<uint8_t>& out, std::string& error,
                  uint64_t, uint64_t) override {
    ++reads;
    lengths.push_back(length);
    if (fail) {
      error = "fake failure";
      return false;
    }
    if (index >= files_.size() || offset >= files_[index].size) return true;
    const uint64_t available = files_[index].size - offset;
    const uint64_t take = std::min<uint64_t>(
        {static_cast<uint64_t>(length), available,
         short_read ? 3u : std::numeric_limits<uint64_t>::max()});
    out.assign(static_cast<std::size_t>(take), 0x5a);
    return true;
  }

  void fail_all(const std::string&) override { closed_ = true; }
  bool closed() const override { return closed_; }

  int reads = 0;
  std::vector<uint32_t> lengths;
  bool fail = false;
  bool short_read = false;

 private:
  std::vector<lazy_clipboard_file> files_;
  bool closed_ = false;
};

/// The request callback fires on the reading thread; the reply is posted from
/// a detached thread, exactly like a reply that arrives on the session loop.
/// The thread owns a shared_ptr so it cannot touch a destroyed source.
std::shared_ptr<remote_file_source> make_remote_source(
    std::vector<lazy_clipboard_file> files,
    std::function<void(remote_file_source&, uint32_t, uint64_t)> reply,
    std::shared_ptr<std::atomic<int>> requests = {}) {
  auto holder = std::make_shared<std::shared_ptr<remote_file_source>>();
  auto source = std::make_shared<remote_file_source>(
      std::move(files),
      [holder, reply = std::move(reply), requests](
          std::size_t, uint32_t id, uint64_t offset, uint32_t) {
        if (requests) ++*requests;
        auto strong = *holder;
        std::thread([strong, reply, id, offset] {
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          reply(*strong, id, offset);
        }).detach();
        return true;
      });
  *holder = source;
  return source;
}

void test_stream_flags_and_eof() {
  auto eager = make_source(make_files(1));
  expect(!eager->streaming(), "the upload source is not a streaming source");

  auto requests = std::make_shared<std::atomic<int>>(0);
  auto remote = make_remote_source(
      make_files(1, 4),
      [](remote_file_source& source, uint32_t id, uint64_t offset) {
        const uint8_t bytes[4] = {'a', 'b', 'c', 'd'};
        source.note_range(id, 0, offset, bytes, sizeof(bytes));
      },
      requests);
  expect(remote->streaming(), "the range source reports streaming");

  std::vector<uint8_t> out;
  std::string error;
  // Reading exactly at EOF must not send a request.
  expect(remote->read_range(0, 4, 4, out, error, 1000, 1000),
         "a read at end of file succeeds");
  expect(out.empty(), "a read at end of file returns no bytes");
  expect(requests->load() == 0, "a read at end of file sends no fetch");
  expect(remote->read_range(0, 9, 4, out, error, 1000, 1000),
         "an offset past end of file is a clean EOF");
  expect(out.empty() && requests->load() == 0,
         "an offset past end of file sends no fetch");
}

void test_remote_range_replies() {
  auto requests = std::make_shared<std::atomic<int>>(0);
  auto remote = make_remote_source(
      make_files(1, 4),
      [](remote_file_source& source, uint32_t id, uint64_t offset) {
        const uint8_t bytes[4] = {'a', 'b', 'c', 'd'};
        source.note_range(id, 0, offset, bytes, sizeof(bytes));
      },
      requests);
  std::vector<uint8_t> out;
  std::string error;
  const bool ok = remote->read_range(0, 0, 4, out, error, 1000, 1000);
  expect(ok, "a matching range reply satisfies read_range");
  expect(out.size() == 4 && std::memcmp(out.data(), "abcd", 4) == 0,
         "the reply bytes are returned");
  expect(requests->load() == 1, "exactly one fetch was sent");
}

void test_remote_error_reply() {
  auto remote = make_remote_source(
      make_files(1, 4),
      [](remote_file_source& source, uint32_t id, uint64_t offset) {
        source.note_range(id, 1, offset, nullptr, 0);
      });
  std::vector<uint8_t> out;
  std::string error;
  const auto started = std::chrono::steady_clock::now();
  const bool ok = remote->read_range(0, 0, 4, out, error, 1000, 1000);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  expect(!ok, "an error reply fails the read");
  expect(elapsed < std::chrono::milliseconds(500),
         "an error reply fails at once, not at the timeout");
}

void test_remote_offset_mismatch() {
  auto remote = make_remote_source(
      make_files(1, 8),
      [](remote_file_source& source, uint32_t id, uint64_t offset) {
        const uint8_t bytes[4] = {1, 2, 3, 4};
        source.note_range(id, 0, offset + 1, bytes, sizeof(bytes));
      });
  std::vector<uint8_t> out;
  std::string error;
  expect(!remote->read_range(0, 0, 4, out, error, 1000, 1000),
         "a reply for another offset is rejected");
}

void test_remote_timeout() {
  auto remote = std::make_shared<remote_file_source>(
      make_files(1, 4),
      [](std::size_t, uint32_t, uint64_t, uint32_t) { return true; });
  std::vector<uint8_t> out;
  std::string error;
  const auto started = std::chrono::steady_clock::now();
  const bool ok = remote->read_range(0, 0, 4, out, error, 60, 200);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  expect(!ok, "a fetch nobody answers times out");
  expect(elapsed >= std::chrono::milliseconds(40),
         "the block budget was actually waited");
  expect(elapsed < std::chrono::seconds(1), "the timeout is bounded");
}

void test_remote_session_loss_wakes_waiter() {
  auto holder = std::make_shared<std::shared_ptr<remote_file_source>>();
  std::atomic<int> requests{0};
  auto remote = std::make_shared<remote_file_source>(
      make_files(1, 4),
      [holder, &requests](std::size_t, uint32_t, uint64_t, uint32_t) {
        ++requests;
        auto strong = *holder;
        std::thread([strong] {
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          strong->fail_all("session closed");
        }).detach();
        return true;
      });
  *holder = remote;
  std::vector<uint8_t> out;
  std::string error;
  const auto started = std::chrono::steady_clock::now();
  const bool ok = remote->read_range(0, 0, 4, out, error, 5000, 5000);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  expect(requests.load() == 1, "the read did ask for its range");
  expect(!ok, "a closed session fails a waiting read");
  expect(elapsed < std::chrono::milliseconds(500),
         "the waiter is woken immediately, not at the timeout");
}

void test_range_reader_boundaries() {
  fake_blob_source source(200000);
  std::vector<uint8_t> buffer(150000);
  uint64_t position = 0;
  uint32_t read = 0;
  std::string error;
  expect(read_lazy_blob(source, 0, position, buffer.data(),
                        static_cast<uint32_t>(buffer.size()), read, error,
                        1000, 10000),
         "a read spanning three blocks succeeds");
  expect(read == 150000 && position == 150000,
         "every byte and the position advanced");
  expect(source.lengths.size() == 3 && source.lengths[0] == 65536 &&
             source.lengths[1] == 65536 && source.lengths[2] == 150000 - 131072,
         "the reader asks 64 KiB at a time and shrinks the last block");

  uint32_t zero = 7;
  position = 5;
  expect(read_lazy_blob(source, 0, position, buffer.data(), 0, zero, error,
                        1000, 1000),
         "a zero-byte read succeeds");
  expect(zero == 0 && position == 5,
         "a zero-byte read requests nothing and changes nothing");

  const int reads_before_eof = source.reads;
  position = 200000;
  read = 9;
  expect(read_lazy_blob(source, 0, position, buffer.data(), 10, read, error,
                        1000, 1000),
         "a read at end of file succeeds");
  expect(read == 0 && position == 200000 && source.reads == reads_before_eof,
         "a read at end of file sends no fetch");

  fake_blob_source short_source(1000);
  short_source.short_read = true;
  position = 0;
  read = 0;
  expect(read_lazy_blob(short_source, 0, position, buffer.data(), 100, read,
                        error, 1000, 1000),
         "a short range is not an error");
  expect(read == 3 && position == 3,
         "a short range ends the read at what arrived");

  fake_blob_source broken(1000);
  broken.fail = true;
  position = 0;
  read = 0;
  expect(!read_lazy_blob(broken, 0, position, buffer.data(), 100, read, error,
                         1000, 1000),
         "a failed range fails the read");
  expect(read == 0, "a failed range produces no bytes");
}

void test_seek_arithmetic() {
  uint64_t target = 0;
  expect(lazy_blob_seek(100, 10, 5, lazy_seek_origin::begin, target) &&
             target == 5,
         "SEEK_SET is absolute");
  expect(lazy_blob_seek(100, 10, 5, lazy_seek_origin::current, target) &&
             target == 15,
         "SEEK_CUR is relative to the current position");
  expect(lazy_blob_seek(100, 10, -5, lazy_seek_origin::end, target) &&
             target == 95,
         "SEEK_END is relative to the file size");
  expect(lazy_blob_seek(100, 10, 500, lazy_seek_origin::begin, target) &&
             target == 500,
         "seeking past the end is allowed");
  expect(!lazy_blob_seek(100, 0, -1, lazy_seek_origin::begin, target),
         "a negative position is rejected");
  expect(!lazy_blob_seek(100, 0, -11, lazy_seek_origin::current, target),
         "a negative position from SEEK_CUR is rejected");
}

}  // namespace

int main() {
  test_offer_limits();
  test_registry_lifecycle();
  test_source_completion();
  test_total_timeout();
  test_chunk_timeout();
  test_session_loss_wakes_waiter();
  test_ensure_all_shares_deadline();
  test_stream_flags_and_eof();
  test_remote_range_replies();
  test_remote_error_reply();
  test_remote_offset_mismatch();
  test_remote_timeout();
  test_remote_session_loss_wakes_waiter();
  test_range_reader_boundaries();
  test_seek_arithmetic();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
