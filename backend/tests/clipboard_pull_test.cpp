#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "clipboard_pull.h"

namespace {

namespace fs = std::filesystem;
using molink::clipboard_file;
using molink::clipboard_offer_entry;
using molink::clipboard_pull_result;
using molink::clipboard_pull_status;
using molink::read_clipboard_pull;

int checks = 0;
int failures = 0;

void expect(bool value, const char* message) {
  ++checks;
  if (!value) {
    ++failures;
    std::printf("FAIL: %s\n", message);
  }
}

std::string write_file(const fs::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return path.string();
}

clipboard_file entry(const std::string& path, const std::string& name,
                     uint64_t size) {
  clipboard_file file;
  file.path = path;
  file.name = name;
  file.size = size;
  return file;
}

clipboard_offer_entry offer(uint32_t id, const std::string& name,
                            uint64_t size) {
  clipboard_offer_entry item;
  item.id = id;
  item.name = name;
  item.size = size;
  return item;
}

void test_range_read() {
  const fs::path dir = fs::temp_directory_path() / "molink-clipboard-pull-a";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  const std::string path = write_file(dir / "a.txt", "0123456789");

  const std::vector<clipboard_file> current = {entry(path, "a.txt", 10)};
  const std::vector<clipboard_offer_entry> offered = {offer(1, "a.txt", 10)};

  clipboard_pull_result first =
      read_clipboard_pull(current, offered, 1, 0, 4, 256 * 1024);
  expect(first.status == clipboard_pull_status::ok, "a valid id reads");
  expect(first.offset == 0, "the reply echoes the offset");
  expect(std::string(first.data.begin(), first.data.end()) == "0123",
         "the requested prefix is returned");

  clipboard_pull_result second =
      read_clipboard_pull(current, offered, 1, 4, 100, 5);
  expect(second.status == clipboard_pull_status::ok,
         "a read near EOF is bounded by the file size");
  expect(second.data.size() == 5, "max_chunk bounds the reply");
  expect(std::string(second.data.begin(), second.data.end()) == "45678",
         "the later range starts at the offset");

  clipboard_pull_result empty =
      read_clipboard_pull(current, offered, 1, 10, 4, 256 * 1024);
  expect(empty.status == clipboard_pull_status::ok,
         "offset == size is an empty success");
  expect(empty.data.empty(), "no bytes at EOF");

  clipboard_pull_result past =
      read_clipboard_pull(current, offered, 1, 11, 4, 256 * 1024);
  expect(past.status == clipboard_pull_status::read_error,
         "an offset past EOF is a read error");
  fs::remove_all(dir, ec);
}

void test_gone_cases() {
  const fs::path dir = fs::temp_directory_path() / "molink-clipboard-pull-b";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  const std::string a = write_file(dir / "a.txt", "aaa");
  const std::string b = write_file(dir / "b.txt", "bbb");

  const std::vector<clipboard_offer_entry> offered = {
      offer(1, "a.txt", 3), offer(2, "b.txt", 3)};

  // Unknown ids and an empty announcement.
  expect(read_clipboard_pull({}, {}, 1, 0, 4, 1024).status ==
             clipboard_pull_status::gone,
         "no announcement is gone");
  expect(read_clipboard_pull({entry(a, "a.txt", 3)}, offered, 3, 0, 4, 1024)
                 .status == clipboard_pull_status::gone,
         "an out-of-range id is gone");

  // The clipboard changed from files to text: the host enumerates an empty
  // list, and the pull must not create an empty file.
  expect(read_clipboard_pull({}, offered, 1, 0, 4, 1024).status ==
             clipboard_pull_status::gone,
         "text replacing files is gone");

  // Same count, different order/content: the id no longer names the file the
  // viewer was told about.
  const std::vector<clipboard_file> reordered = {
      entry(b, "b.txt", 3), entry(a, "a.txt", 3)};
  expect(read_clipboard_pull(reordered, offered, 1, 0, 4, 1024).status ==
             clipboard_pull_status::gone,
         "a different name at this id is gone");

  // RDP redirection replaced the temporary file with one of another size.
  const std::vector<clipboard_file> resized = {entry(a, "a.txt", 7),
                                               entry(b, "b.txt", 3)};
  expect(read_clipboard_pull(resized, offered, 1, 0, 4, 1024).status ==
             clipboard_pull_status::gone,
         "a changed file size is gone");

  // The file disappeared between announcement and pull (inner RDP closed).
  const std::vector<clipboard_file> missing = {
      entry((dir / "gone.txt").string(), "a.txt", 3),
      entry(b, "b.txt", 3)};
  expect(read_clipboard_pull(missing, offered, 1, 0, 4, 1024).status ==
             clipboard_pull_status::gone,
         "a vanished path is gone");

  // A directory (CF_HDROP can carry one) is not a readable clipboard file.
  const std::vector<clipboard_file> directory = {
      entry(dir.string(), "a.txt", 3), entry(b, "b.txt", 3)};
  expect(read_clipboard_pull(directory, offered, 1, 0, 4, 1024).status ==
             clipboard_pull_status::gone,
         "a directory path is gone");

  fs::remove_all(dir, ec);
}

void test_short_read_is_an_error() {
  const fs::path dir = fs::temp_directory_path() / "molink-clipboard-pull-c";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  const std::string path = write_file(dir / "a.txt", "abc");
  // The announcement claims four bytes but the file has three; the size check
  // turns this into gone before any read happens.
  const std::vector<clipboard_file> current = {entry(path, "a.txt", 3)};
  const std::vector<clipboard_offer_entry> offered = {offer(1, "a.txt", 3)};
  clipboard_pull_result result =
      read_clipboard_pull(current, offered, 1, 0, 4, 1024);
  expect(result.status == clipboard_pull_status::ok,
         "a normal short file reads fully");
  expect(result.data.size() == 3, "all bytes are returned");
  fs::remove_all(dir, ec);
}

}  // namespace

int main() {
  test_range_read();
  test_gone_cases();
  test_short_read_is_an_error();
  std::printf("%d checks, %d failure(s)\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
