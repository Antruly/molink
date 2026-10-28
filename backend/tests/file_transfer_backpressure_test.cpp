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
  transfer.pull(1);
  expect(attempts == 1, "first chunk send is attempted");
  transfer.pull(1);
  expect(attempts == 2 && offsets.size() == 2 && offsets[0] == 0 &&
             offsets[1] == 0,
         "failed enqueue retries from the unchanged offset");
  accept_send = true;
  transfer.pull(1);
  expect(attempts == 3 && offsets.back() == 0,
         "chunk is resent from offset zero after backpressure clears");
  transfer.pull(1);
  expect(attempts == 4 && offsets.back() == molink::file_transfer::kChunkBytes,
         "next chunk starts only after accepted data");
  transfer.close_all();
  fs::remove_all(home, ec);
}
}  // namespace

int main() {
  test_download_retries_same_offset_after_backpressure();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
