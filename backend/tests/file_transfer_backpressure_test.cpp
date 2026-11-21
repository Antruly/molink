#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "file_transfer.h"

namespace {
int checks = 0;
int failures = 0;
void expect(bool value, const char* message) {
  ++checks;
  if (!value) {
    ++failures;
    std::printf("FAIL: %s\n", message);
  }
}

void test_download_retries_same_offset_after_backpressure() {
  namespace fs = std::filesystem;
  const fs::path home = fs::temp_directory_path() / "molink-file-transfer-test";
  std::error_code ec;
  fs::remove_all(home, ec);
  fs::create_directories(home, ec);
#if defined(_WIN32)
  _putenv_s("USERPROFILE", home.string().c_str());
#else
  setenv("HOME", home.string().c_str(), 1);
#endif
  const fs::path source = home / "payload.bin";
  {
    std::ofstream file(source, std::ios::binary);
    file << std::string(molink::file_transfer::kChunkBytes + 4, 'x');
  }

  bool accept_send = false;
  int attempts = 0;
  std::vector<uint64_t> offsets;
  std::vector<std::string> json;
  molink::file_transfer transfer(
      [&json](std::string value) { json.push_back(std::move(value)); },
      [&accept_send, &attempts, &offsets](uint32_t, uint64_t offset,
                                          const uint8_t*, std::size_t) {
        ++attempts;
        offsets.push_back(offset);
        return accept_send;
      });
  transfer.request("payload.bin");
  expect(!json.empty(), "request announces the download");
  const auto start = molink::file_transfer::clock::now();
  transfer.pull(1, start);
  expect(attempts == 1 && transfer.has_pending(), "rejected pull stays pending");
  transfer.pull(1, start);
  expect(attempts == 1, "duplicate pull does not advance a pending request");
  transfer.retry_pending(start + std::chrono::milliseconds(100));
  expect(attempts == 2 && offsets[0] == 0 && offsets[1] == 0,
         "retry tick attempts the same offset");
  accept_send = true;
  transfer.retry_pending(start + std::chrono::milliseconds(200));
  expect(attempts == 3 && offsets.back() == 0 && !transfer.has_pending(),
         "the first block is delivered without another client pull");
  transfer.retry_pending(start + std::chrono::milliseconds(300));
  expect(attempts == 3, "the next block still needs a new pull");
  transfer.pull(1);
  expect(attempts == 4 && offsets.back() == molink::file_transfer::kChunkBytes,
         "next chunk starts only after accepted data");

  accept_send = false;
  transfer.request("payload.bin");
  transfer.pull(2, start);
  transfer.cancel(2);
  transfer.retry_pending(start + std::chrono::milliseconds(100));
  expect(attempts == 5 && !transfer.has_pending(), "cancel clears the retry");

  transfer.request("payload.bin");
  transfer.pull(3, start);
  transfer.retry_pending(start + std::chrono::seconds(19));
  const auto before_timeout = json.size();
  transfer.retry_pending(start + molink::file_transfer::kRetryTimeout);
  expect(attempts == 7 && !transfer.has_pending() &&
             json.size() == before_timeout + 1 &&
             json.back().find("did not become writable") != std::string::npos,
         "retry deadline reports failure without sending another block");

  transfer.request("payload.bin");
  transfer.pull(4, start);
  transfer.close_all();
  transfer.retry_pending(start + std::chrono::milliseconds(100));
  expect(attempts == 8 && !transfer.has_pending(), "close clears the retry");

  transfer.offer("upload.bin", 4, home.string());
  const auto before_complete = json.size();
  transfer.complete(5, start);
  expect(transfer.has_pending() && json.size() == before_complete &&
             !fs::exists(home / "upload.bin"),
         "early complete does not publish an incomplete upload");
  const std::array<uint8_t, 4> content{'t', 'e', 's', 't'};
  transfer.chunk(5, 0, content.data(), content.size());
  std::ifstream uploaded(home / "upload.bin", std::ios::binary);
  std::array<char, 4> received{};
  uploaded.read(received.data(), received.size());
  expect(!transfer.has_pending() && json.size() == before_complete + 1 &&
             std::string(received.data(), received.size()) == "test",
         "an upload with early complete is saved only after all bytes arrive");
  uploaded.close();

  transfer.offer("short.bin", 4, home.string());
  transfer.complete(6, start);
  transfer.retry_pending(start + molink::file_transfer::kRetryTimeout);
  expect(!transfer.has_pending() && !fs::exists(home / "short.bin") &&
             !fs::exists(home / ".upload-6.part") &&
             json.back().find("file ended early") != std::string::npos,
         "incomplete upload times out and its temporary file is removed");
  fs::remove_all(home, ec);
}
void test_download_and_upload_slots_are_reclaimed() {
  namespace fs = std::filesystem;
  const fs::path home =
      fs::temp_directory_path() / "molink-file-transfer-idle-test";
  std::error_code ec;
  fs::remove_all(home, ec);
  fs::create_directories(home, ec);
#if defined(_WIN32)
  _putenv_s("USERPROFILE", home.string().c_str());
#else
  setenv("HOME", home.string().c_str(), 1);
#endif
  const fs::path source = home / "payload.bin";
  {
    std::ofstream file(source, std::ios::binary);
    file << std::string(64, 'x');
  }

  std::vector<std::string> json;
  const auto accept_chunk = [](uint32_t, uint64_t, const uint8_t*,
                               std::size_t) { return true; };
  molink::file_transfer transfer(
      [&json](std::string value) { json.push_back(std::move(value)); },
      accept_chunk);
  const auto start = molink::file_transfer::clock::now();

  // The fixed slot bound: eight open downloads, the ninth is refused.
  for (int index = 0; index < 8; ++index) {
    transfer.request("payload.bin", start);
  }
  expect(json.size() == 8, "eight downloads are accepted");
  transfer.request("payload.bin", start);
  expect(json.size() == 9 &&
             json.back().find("too many active downloads") != std::string::npos,
         "the ninth download is refused while eight are open");
  expect(transfer.has_transfers() && !transfer.has_pending(),
         "an idle download keeps the retry timer alive without a retry deadline");

  // cancel releases the slot immediately.
  transfer.cancel(3);
  transfer.request("payload.bin", start);
  expect(json.size() == 11 &&
             json.back().find("too many active downloads") ==
                 std::string::npos,
         "cancel releases a download slot for the next request");

  // An abandoned download (panel closed without cancel) ages out after the
  // idle timeout. Only the old one is reclaimed: the seconds-older requests
  // keep the queue full, so the slot count is back at eight.
  std::vector<std::string> idle_json;
  molink::file_transfer idle(
      [&idle_json](std::string value) {
        idle_json.push_back(std::move(value));
      },
      accept_chunk);
  const auto idle_start = molink::file_transfer::clock::now();
  idle.request("payload.bin", idle_start);
  const auto later = idle_start + molink::file_transfer::kIdleTimeout / 2;
  for (int index = 0; index < 7; ++index) {
    idle.request("payload.bin", later);
  }
  idle.request("payload.bin", later);
  expect(idle_json.back().find("too many active downloads") !=
             std::string::npos,
         "idle queue is full before the reclaim");
  idle.retry_pending(idle_start + molink::file_transfer::kIdleTimeout +
                     std::chrono::milliseconds(1));
  idle.request("payload.bin",
               idle_start + molink::file_transfer::kIdleTimeout +
                   std::chrono::milliseconds(1));
  expect(idle_json.back().find("too many active downloads") ==
             std::string::npos,
         "one idle download times out and frees its slot");
  idle.request("payload.bin",
               idle_start + molink::file_transfer::kIdleTimeout +
                   std::chrono::milliseconds(1));
  expect(idle_json.back().find("too many active downloads") !=
             std::string::npos,
         "exactly one slot was reclaimed, not the whole queue");

  // Abandoned uploads are reclaimed the same way, and their partial file is
  // removed.
  std::vector<std::string> upload_json;
  molink::file_transfer uploads(
      [&upload_json](std::string value) {
        upload_json.push_back(std::move(value));
      },
      accept_chunk);
  const auto upload_start = molink::file_transfer::clock::now();
  for (int index = 0; index < 8; ++index) {
    uploads.offer("upload.bin", 0, home.string());
  }
  uploads.offer("upload.bin", 0, home.string());
  expect(upload_json.back().find("too many active uploads") !=
             std::string::npos,
         "the ninth upload is refused while eight are open");
  expect(fs::exists(home / ".upload-1.part"),
         "an open upload owns its partial file");
  uploads.retry_pending(upload_start + molink::file_transfer::kIdleTimeout +
                        std::chrono::milliseconds(1));
  expect(!fs::exists(home / ".upload-1.part"),
         "an idle upload's partial file is removed on reclaim");
  uploads.offer("upload.bin", 0, home.string());
  expect(upload_json.back().find("too many active uploads") ==
             std::string::npos,
         "the reclaimed upload slot can be reused");

  fs::remove_all(home, ec);
}

}  // namespace

int main() {
  test_download_retries_same_offset_after_backpressure();
  test_download_and_upload_slots_are_reclaimed();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
