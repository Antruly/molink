#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "clipboard_virtual.h"
#include "clipboard/virtual_mount.h"

namespace {

namespace fs = std::filesystem;
using molink::virtual_clipboard;
using molink::virtual_mount;
using molink::virtual_mount_file;
using molink::virtual_offer_error;
using molink::virtual_offer_request_file;

int checks = 0;
int failures = 0;

void expect(bool value, const char* message) {
  ++checks;
  if (!value) {
    ++failures;
    std::printf("FAIL: %s\n", message);
  }
}

struct mount_state {
  std::string path;
  std::vector<virtual_mount_file> files;
  std::atomic<int> stops{0};
  std::atomic<bool> mounted{true};
};

class fake_mount final : public virtual_mount {
 public:
  explicit fake_mount(std::shared_ptr<mount_state> state)
      : state_(std::move(state)) {}
  const std::string& path() const override { return state_->path; }
  bool mounted() const override { return state_->mounted.load(); }
  bool stop() override {
    if (state_->mounted.exchange(false)) ++state_->stops;
    return true;
  }

 private:
  std::shared_ptr<mount_state> state_;
};

class fake_bridge {
 public:
  std::string wait_message(int timeout_ms = 3000) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                      [this] { return !message_.empty(); })) {
      return std::string();
    }
    const std::string out = message_;
    message_.clear();
    return out;
  }

  void send(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    message_ = message;
    cv_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::string message_;
};

std::vector<uint8_t> make_chunk(uint32_t seq, uint32_t status, uint64_t offset,
                                const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> out(16 + payload.size());
  auto put32 = [&out](std::size_t at, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
      out[at + static_cast<std::size_t>(i)] =
          static_cast<uint8_t>((value >> (8 * i)) & 0xff);
    }
  };
  auto put64 = [&out](std::size_t at, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
      out[at + static_cast<std::size_t>(i)] =
          static_cast<uint8_t>((value >> (8 * i)) & 0xff);
    }
  };
  put32(0, seq);
  put32(4, status);
  put64(8, offset);
  for (std::size_t i = 0; i < payload.size(); ++i) out[16 + i] = payload[i];
  return out;
}

uint32_t parse_seq(const std::string& json) {
  const std::string key = "\"seq\":";
  const std::size_t at = json.find(key);
  if (at == std::string::npos) return 0;
  return static_cast<uint32_t>(std::stoul(json.substr(at + key.size())));
}

void plan_caps_and_names() {
  std::vector<virtual_offer_request_file> requested;
  for (uint32_t i = 1; i <= 10; ++i) {
    virtual_offer_request_file file;
    file.id = i;
    file.name = "file" + std::to_string(i) + ".txt";
    file.size = 10;
    requested.push_back(file);
  }
  requested[0].name = "../escape";
  requested[1].name = "same.txt";
  requested[2].name = "same.txt";
  std::vector<virtual_mount_file> planned;
  std::size_t omitted = 0;
  expect(molink::plan_virtual_offer(requested, planned, omitted) ==
             virtual_offer_error::none,
         "a normal list is accepted");
  expect(planned.size() == molink::kVirtualMaxFiles,
         "at most eight files are kept");
  expect(omitted == requested.size() - planned.size(),
         "the rest are reported as omitted");
  expect(planned[0].name == "same.txt", "the first duplicate keeps its name");
  expect(planned[1].name == "same (1).txt", "duplicates get a suffix");
  for (const virtual_mount_file& entry : planned) {
    expect(entry.name.find('/') == std::string::npos &&
               entry.name.find('\\') == std::string::npos,
           "no separators survive into the mount");
  }

  std::vector<virtual_offer_request_file> large;
  virtual_offer_request_file big;
  big.id = 1;
  big.name = "big.bin";
  big.size = molink::kVirtualMaxFileBytes + 1;
  large.push_back(big);
  omitted = 0;
  planned.clear();
  expect(molink::plan_virtual_offer(large, planned, omitted) ==
             virtual_offer_error::bad_file,
         "an oversized sole file leaves nothing to mount");
  expect(omitted == 1, "the oversized entry counts as omitted");

  std::vector<virtual_offer_request_file> bad;
  virtual_offer_request_file invalid;
  invalid.id = 0;
  invalid.name = "x";
  invalid.size = 1;
  bad.push_back(invalid);
  omitted = 0;
  planned.clear();
  expect(molink::plan_virtual_offer(bad, planned, omitted) ==
             virtual_offer_error::bad_file,
         "a zero id is rejected");
}

void mount_lifecycle() {
  const fs::path root =
      fs::temp_directory_path() / "molink-clipboard-virtual-test";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);

  std::vector<std::shared_ptr<mount_state>> mounts;
  auto factory = [&mounts](const std::string& mountpoint,
                           const std::vector<virtual_mount_file>& files,
                           molink::virtual_file_reader*,
                           std::string&) -> std::shared_ptr<virtual_mount> {
    auto state = std::make_shared<mount_state>();
    state->path = mountpoint;
    state->files = files;
    mounts.push_back(state);
    return std::make_shared<fake_mount>(state);
  };
  std::vector<std::vector<std::string>> applied;
  int clears = 0;
  auto clipboard = std::make_shared<virtual_clipboard>(
      root.string(), factory,
      [&applied](const std::vector<std::string>& paths) {
        applied.push_back(paths);
        return true;
      },
      [&clears] { ++clears; });

  std::vector<virtual_offer_request_file> first;
  virtual_offer_request_file a;
  a.id = 1;
  a.name = "a.txt";
  a.size = 5;
  first.push_back(a);

  std::size_t omitted = 0;
  std::string mount;
  std::vector<std::string> paths;
  std::string error;
  expect(clipboard->offer("s1", first, omitted, mount, paths, error) ==
             virtual_offer_error::none,
         "the first offer mounts");
  expect(mount == "1", "the first generation is numbered from one");
  expect(paths.size() == 1, "one path is published");
  expect(paths[0] == (root / "1" / "a.txt").string(),
         "the clipboard path points into the mount");
  expect(applied.size() == 1, "the clipboard was given the paths");
  expect(mounts.size() == 1 && mounts[0]->stops == 0, "the mount is running");
  expect(fs::is_directory(root / "1", ec), "the mount directory exists");
  expect(clipboard->active_mount() == "1", "the generation is active");

  std::vector<virtual_offer_request_file> second = first;
  second[0].id = 2;
  second[0].name = "b.txt";
  expect(clipboard->offer("s2", second, omitted, mount, paths, error) ==
             virtual_offer_error::none,
         "a new announcement mounts");
  expect(mounts.size() == 2, "a second mount was created");
  expect(mounts[0]->stops == 1, "the previous mount was stopped");
  expect(!fs::exists(root / "1", ec), "the previous directory was removed");
  expect(fs::is_directory(root / "2", ec), "the new directory exists");
  expect(clears == 1, "replacing a generation clears the old clipboard");
  expect(clipboard->active_mount() == "2", "the new generation is active");

  clipboard->clear("test");
  expect(mounts[1]->stops == 1, "clear stops the current mount");
  expect(clears == 2, "clear releases the clipboard");
  expect(!fs::exists(root / "2", ec), "clear removes the directory");
  expect(!clipboard->active(), "nothing is active after clear");

  fs::remove_all(root, ec);
}

void read_paths() {
  const fs::path root =
      fs::temp_directory_path() / "molink-clipboard-virtual-read";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);

  auto factory = [](const std::string& mountpoint,
                    const std::vector<virtual_mount_file>&,
                    molink::virtual_file_reader*,
                    std::string&) -> std::shared_ptr<virtual_mount> {
    auto state = std::make_shared<mount_state>();
    state->path = mountpoint;
    return std::make_shared<fake_mount>(state);
  };
  auto clipboard = std::make_shared<virtual_clipboard>(
      root.string(), factory,
      [](const std::vector<std::string>&) { return true; }, [] {});

  std::vector<virtual_offer_request_file> files;
  virtual_offer_request_file file;
  file.id = 7;
  file.name = "seven.bin";
  file.size = 1000;
  files.push_back(file);
  std::size_t omitted = 0;
  std::string mount;
  std::vector<std::string> paths;
  std::string error;
  expect(clipboard->offer("s", files, omitted, mount, paths, error) ==
             virtual_offer_error::none,
         "read fixture mounts");

  fake_bridge bridge;
  expect(clipboard->bridge_attach(mount,
                                  [&bridge](const std::string& message) {
                                    bridge.send(message);
                                  }),
         "the bridge attaches");

  std::vector<uint8_t> payload(1000);
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<uint8_t>(i & 0xff);
  }
  std::vector<char> buffer(1000, 0);
  int read = 0;
  std::thread reader([&] {
    read = clipboard->read_file_range(7, 0, buffer.size(), buffer.data(),
                                      buffer.size());
  });
  const std::string message = bridge.wait_message();
  expect(!message.empty(), "a read becomes a pull message on the bridge");
  const uint32_t seq = parse_seq(message);
  expect(seq != 0, "the pull carries a sequence number");
  const std::vector<uint8_t> chunk = make_chunk(seq, 0, 0, payload);
  clipboard->bridge_binary(chunk.data(), chunk.size());
  reader.join();
  expect(read == 1000, "a successful reply fills the read");
  expect(std::string(buffer.begin(), buffer.end()) ==
             std::string(payload.begin(), payload.end()),
         "the bytes match the host reply");

  // The kernel may read the same range twice (readahead plus an explicit
  // read); the immutable mount answers the repeat without another pull.
  std::fill(buffer.begin(), buffer.end(), 0);
  read = clipboard->read_file_range(7, 0, buffer.size(), buffer.data(),
                                    buffer.size());
  expect(read == 1000, "a repeated range is served from the cache");
  expect(std::string(buffer.begin(), buffer.end()) ==
             std::string(payload.begin(), payload.end()),
         "the cached bytes match");
  expect(bridge.wait_message(200).empty(),
         "a cached range sends no second pull");

  // A clipboard change (status 2) must map to EIO, not a short file. Use a
  // different length so the successful range above is not served from cache.
  std::thread failing([&] {
    read = clipboard->read_file_range(7, 0, 500, buffer.data(), 500);
  });
  const std::string message2 = bridge.wait_message();
  const uint32_t seq2 = parse_seq(message2);
  const std::vector<uint8_t> gone = make_chunk(seq2, 2, 0, {});
  clipboard->bridge_binary(gone.data(), gone.size());
  failing.join();
  expect(read < 0, "status 2 is reported as a read failure");

  // A bridge that disappears while a read is waiting must fail that read.
  std::thread detached([&] {
    read = clipboard->read_file_range(7, 0, 400, buffer.data(), 400);
  });
  expect(!bridge.wait_message().empty(), "the detached read was dispatched");
  clipboard->bridge_detach(mount);
  detached.join();
  expect(read < 0, "a vanished bridge fails the pending read");
  expect(!clipboard->active(), "a vanished bridge unmounts the generation");

  fs::remove_all(root, ec);
}

/// Every failure of a FUSE range read must be -EIO, and a page-side send that
/// throws must not escape into the FUSE worker (which would terminate the
/// core). The production timeouts are 15 s/60 s; the test injects short ones.
void read_failure_paths() {
  const fs::path root =
      fs::temp_directory_path() / "molink-clipboard-virtual-read-fail";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);

  auto factory = [](const std::string& mountpoint,
                    const std::vector<virtual_mount_file>&,
                    molink::virtual_file_reader*,
                    std::string&) -> std::shared_ptr<virtual_mount> {
    auto state = std::make_shared<mount_state>();
    state->path = mountpoint;
    return std::make_shared<fake_mount>(state);
  };
  auto clipboard = std::make_shared<virtual_clipboard>(
      root.string(), factory,
      [](const std::vector<std::string>&) { return true; }, [] {}, nullptr,
      150, 150);

  std::vector<virtual_offer_request_file> files;
  virtual_offer_request_file file;
  file.id = 7;
  file.name = "seven.bin";
  file.size = 1000;
  files.push_back(file);
  std::size_t omitted = 0;
  std::string mount;
  std::vector<std::string> paths;
  std::string error;
  expect(clipboard->offer("s", files, omitted, mount, paths, error) ==
             virtual_offer_error::none,
         "failure fixture mounts");

  std::vector<char> buffer(1000, 0);
  int read = 0;

  // No bridge at all: fail while the paste has not started.
  read = clipboard->read_file_range(7, 0, buffer.size(), buffer.data(),
                                    buffer.size());
  expect(read < 0, "a read without a bridge is EIO");

  // A bridge whose send throws. The exception is raised synchronously from
  // pull_block; read_file_range must translate it to EIO.
  {
    auto started = std::chrono::steady_clock::now();
    expect(clipboard->bridge_attach(
               mount, [](const std::string&) {
                 throw std::runtime_error("bridge send threw");
               }),
           "the throwing bridge attaches");
    read = clipboard->read_file_range(7, 0, 400, buffer.data(), buffer.size());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    expect(read < 0, "a throwing bridge send is EIO");
    expect(elapsed < std::chrono::seconds(1),
           "a throwing send fails at once, not at the timeout");
  }

  // A reply for another offset is rejected; the read wakes immediately.
  fake_bridge bridge;
  expect(clipboard->bridge_attach(mount,
                                  [&bridge](const std::string& message) {
                                    bridge.send(message);
                                  }),
         "the recording bridge attaches");
  {
    std::thread reader([&] {
      read = clipboard->read_file_range(7, 0, 300, buffer.data(),
                                        buffer.size());
    });
    const std::string message = bridge.wait_message();
    expect(!message.empty(), "the read reached the bridge");
    const uint32_t seq = parse_seq(message);
    std::vector<uint8_t> payload(300, 0x11);
    const std::vector<uint8_t> wrong =
        make_chunk(seq, 0, 1, payload);
    clipboard->bridge_binary(wrong.data(), wrong.size());
    reader.join();
    expect(read < 0, "a reply at the wrong offset is EIO");
  }

  // A short reply must not be treated as end of file: the descriptor said
  // 1000 bytes, and a partial answer would create a truncated paste.
  {
    std::thread reader([&] {
      read = clipboard->read_file_range(7, 300, 200, buffer.data(),
                                        buffer.size());
    });
    const std::string message = bridge.wait_message();
    const uint32_t seq = parse_seq(message);
    std::vector<uint8_t> short_payload(199, 0x22);
    const std::vector<uint8_t> short_reply =
        make_chunk(seq, 0, 300, short_payload);
    clipboard->bridge_binary(short_reply.data(), short_reply.size());
    reader.join();
    expect(read < 0, "a short reply is EIO, not a short file");
  }

  // Nobody answers: the injected 150 ms block budget fails the read.
  {
    fake_bridge silent;
    expect(clipboard->bridge_attach(mount,
                                    [&silent](const std::string& message) {
                                      silent.send(message);
                                    }),
           "the silent bridge attaches");
    std::thread reader([&] {
      read = clipboard->read_file_range(7, 500, 100, buffer.data(),
                                        buffer.size());
    });
    const auto started = std::chrono::steady_clock::now();
    reader.join();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    expect(read < 0, "a stalled bridge times out as EIO");
    expect(elapsed < std::chrono::seconds(2),
           "the injected read timeout is honoured");
  }

  // A failed read must not leave a waiter behind, so clearing the generation
  // returns without the 500 ms drain window.
  {
    const auto started = std::chrono::steady_clock::now();
    clipboard->clear("test");
    const auto elapsed = std::chrono::steady_clock::now() - started;
    expect(!clipboard->active(), "clear drops the generation");
    expect(elapsed < std::chrono::milliseconds(400),
           "a failed read decrements the waiting-reader count");
  }

  fs::remove_all(root, ec);
}

/// The publisher noticed that another application replaced the clipboard.
/// Every blocked read must wake now, and later reads must fail at once: a
/// stale offer must never leave a paste waiting for the 15 s budget.
void cancelled_reads() {
  const fs::path root =
      fs::temp_directory_path() / "molink-clipboard-virtual-cancel";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);

  auto factory = [](const std::string& mountpoint,
                    const std::vector<virtual_mount_file>&,
                    molink::virtual_file_reader*,
                    std::string&) -> std::shared_ptr<virtual_mount> {
    auto state = std::make_shared<mount_state>();
    state->path = mountpoint;
    return std::make_shared<fake_mount>(state);
  };
  auto clipboard = std::make_shared<virtual_clipboard>(
      root.string(), factory,
      [](const std::vector<std::string>&) { return true; }, [] {}, nullptr,
      5000, 5000);

  std::vector<virtual_offer_request_file> files;
  virtual_offer_request_file file;
  file.id = 7;
  file.name = "seven.bin";
  file.size = 1000;
  files.push_back(file);
  std::size_t omitted = 0;
  std::string mount;
  std::vector<std::string> paths;
  std::string error;
  expect(clipboard->offer("s", files, omitted, mount, paths, error) ==
             virtual_offer_error::none,
         "cancel fixture mounts");

  fake_bridge bridge;
  expect(clipboard->bridge_attach(mount,
                                  [&bridge](const std::string& message) {
                                    bridge.send(message);
                                  }),
         "cancel fixture bridge attaches");

  std::vector<char> buffer(300, 0);
  int read = 0;
  std::thread reader([&] {
    read = clipboard->read_file_range(7, 0, buffer.size(), buffer.data(),
                                      buffer.size());
  });
  expect(!bridge.wait_message().empty(), "the read was dispatched");
  clipboard->cancel_reads();
  reader.join();
  expect(read < 0, "cancel_reads fails the pending read");

  read = clipboard->read_file_range(7, 500, 100, buffer.data(), 100);
  expect(read < 0, "later reads fail immediately");
  expect(bridge.wait_message(200).empty(),
         "no further pull is sent after cancellation");

  // A new generation is allowed to serve again.
  std::vector<virtual_offer_request_file> second = files;
  second[0].id = 8;
  second[0].name = "eight.bin";
  expect(clipboard->offer("s", second, omitted, mount, paths, error) ==
             virtual_offer_error::none,
         "a new generation mounts after cancellation");
  expect(clipboard->bridge_attach(mount,
                                  [&bridge](const std::string& message) {
                                    bridge.send(message);
                                  }),
         "the new generation's bridge attaches");
  std::thread again([&] {
    read = clipboard->read_file_range(8, 0, 100, buffer.data(), 100);
  });
  const std::string message = bridge.wait_message();
  expect(!message.empty(), "the new generation sends a pull");
  if (!message.empty()) {
    const uint32_t seq = parse_seq(message);
    const std::vector<uint8_t> payload(100, 0x33);
    const std::vector<uint8_t> reply = make_chunk(seq, 0, 0, payload);
    clipboard->bridge_binary(reply.data(), reply.size());
  }
  again.join();
  expect(read == 100, "the new generation serves bytes");

  clipboard->clear("test");
  fs::remove_all(root, ec);
}

}  // namespace

// The test links no platform mount layer: the fake mount always detaches, so
// the stale-directory guard is never taken.
namespace molink {
bool platform_path_is_mountpoint(const std::string&) { return false; }
}  // namespace molink

int main() {
  plan_caps_and_names();
  mount_lifecycle();
  read_paths();
  read_failure_paths();
  cancelled_reads();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
