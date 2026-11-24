#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "file_transfer.h"
#include "sha256.h"

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

/// The partial file for upload @p id, whatever the per-instance tag is.
std::filesystem::path find_partial(const std::filesystem::path& home,
                                   const std::string& suffix) {
  namespace fs = std::filesystem;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(home, ec)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind(".upload-", 0) == 0 && name.size() > suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
      return entry.path();
    }
  }
  return fs::path();
}

std::size_t count_partials(const std::filesystem::path& home) {
  namespace fs = std::filesystem;
  std::size_t count = 0;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(home, ec)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind(".upload-", 0) == 0) ++count;
  }
  return count;
}

/// The compact JSON dump writes `"name":N` (with one space after the colon),
/// so pull a number without a full parser.
uint64_t json_number(const std::string& message, const std::string& field) {
  std::size_t pos = message.find("\"" + field + "\"");
  if (pos == std::string::npos) return 0;
  pos = message.find(':', pos);
  if (pos == std::string::npos) return 0;
  ++pos;
  while (pos < message.size() &&
         (message[pos] == ' ' || message[pos] == '\t')) {
    ++pos;
  }
  uint64_t value = 0;
  while (pos < message.size() && message[pos] >= '0' && message[pos] <= '9') {
    value = value * 10 + static_cast<uint64_t>(message[pos] - '0');
    ++pos;
  }
  return value;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

void set_home(const std::filesystem::path& home) {
#if defined(_WIN32)
  _putenv_s("USERPROFILE", home.string().c_str());
#else
  setenv("HOME", home.string().c_str(), 1);
#endif
}

void remove_tree(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
}

std::filesystem::path fresh_home(const char* name) {
  namespace fs = std::filesystem;
  const fs::path home = fs::temp_directory_path() / name;
  std::error_code ec;
  fs::remove_all(home, ec);
  fs::create_directories(home, ec);
  set_home(home);
  return home;
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
  // A completing chunk now also emits `file-ack` before `done`, so the exact
  // message count is not the contract: the file lands and `done` is last.
  expect(!transfer.has_pending() && json.size() > before_complete &&
             json.back().find("\"done\"") != std::string::npos &&
             std::string(received.data(), received.size()) == "test",
         "an upload with early complete is saved only after all bytes arrive");
  uploaded.close();

  transfer.offer("short.bin", 4, home.string());
  transfer.complete(6, start);
  transfer.retry_pending(start + molink::file_transfer::kRetryTimeout);
  expect(!transfer.has_pending() && !fs::exists(home / "short.bin") &&
             !fs::exists(find_partial(home, "-6.part")) &&
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
  // The partial name carries a per-instance tag so two cores (or two sessions)
  // cannot collide on `.upload-1.part`.
  const fs::path first_partial = find_partial(home, "-1.part");
  expect(!first_partial.empty(), "an open upload owns its partial file");
  uploads.retry_pending(upload_start + molink::file_transfer::kIdleTimeout +
                        std::chrono::milliseconds(1));
  expect(!fs::exists(first_partial),
         "an idle upload's partial file is removed on reclaim");
  uploads.offer("upload.bin", 0, home.string());
  expect(upload_json.back().find("too many active uploads") ==
             std::string::npos,
         "the reclaimed upload slot can be reused");

  fs::remove_all(home, ec);
}

/// The compact JSON dump writes `"id":N`, so pull the number without a parser.
uint32_t id_in(const std::string& message) {
  std::size_t pos = message.find("\"id\"");
  if (pos == std::string::npos) return 0;
  pos = message.find(':', pos);
  if (pos == std::string::npos) return 0;
  ++pos;
  while (pos < message.size() &&
         (message[pos] == ' ' || message[pos] == '\t')) {
    ++pos;
  }
  uint32_t value = 0;
  while (pos < message.size() && message[pos] >= '0' && message[pos] <= '9') {
    value = value * 10 + static_cast<uint32_t>(message[pos] - '0');
    ++pos;
  }
  return value;
}

void test_upload_paths_and_mkdir() {
  namespace fs = std::filesystem;
  const fs::path home =
      fs::temp_directory_path() / "molink-file-transfer-paths-test";
  std::error_code ec;
  fs::remove_all(home, ec);
  fs::create_directories(home, ec);
#if defined(_WIN32)
  _putenv_s("USERPROFILE", home.string().c_str());
#else
  setenv("HOME", home.string().c_str(), 1);
#endif
  std::vector<std::string> json;
  molink::file_transfer transfer(
      [&json](std::string value) { json.push_back(std::move(value)); },
      [](uint32_t, uint64_t, const uint8_t*, std::size_t) { return true; });

  auto accept = [&](const std::string& name, uint64_t size) {
    transfer.offer(name, size, home.string());
    const uint32_t id = id_in(json.back());
    expect(id != 0,
           "a relative upload name is accepted");
    return id;
  };
  auto finish = [&](uint32_t id, const std::string& text) {
    std::vector<uint8_t> bytes(text.begin(), text.end());
    transfer.chunk(id, 0, bytes.data(), bytes.size());
    transfer.complete(id);
  };

  // A client that has no directory support sends only a bare file name; that
  // path must behave exactly as before.
  finish(accept("plain.txt", 5), "hello");
  expect(fs::exists(home / "plain.txt"),
         "a bare file name still lands in the destination");

  // A folder drag sends the relative parents and they are recreated.
  finish(accept("photos/2024/a.jpg", 4), "jpeg");
  expect(fs::exists(home / "photos" / "2024" / "a.jpg"),
         "a relative path creates its folders under the destination");

  finish(accept("照片/旅行/海.jpg", 4), "sea!");
  expect(fs::exists(home / "照片" / "旅行" / "海.jpg"),
         "UTF-8 folder and file names survive");

  // Two files with the same relative name stay distinct.
  finish(accept("dup.txt", 1), "a");
  finish(accept("dup.txt", 1), "b");
  expect(fs::exists(home / "dup.txt") && fs::exists(home / "dup (1).txt"),
         "a duplicate name gets a numbered sibling");

  // Absolute paths, drive letters, traversal and reserved device names are
  // all refused before an upload slot or a temporary file is created.
  const char* bad_names[] = {
      "/etc/passwd", "C:\\Windows\\evil.txt", "\\\\server\\share\\x",
      "..\\escape.txt", "a/../../escape.txt", "CON.txt", "nul", "a//b.txt"};
  for (const char* name : bad_names) {
    const std::size_t count = json.size();
    transfer.offer(name, 1, home.string());
    expect(json.size() == count + 1 &&
               json.back().find("invalid file name") != std::string::npos,
           "an unsafe upload path is rejected");
  }
  expect(!fs::exists(home / "etc" / "passwd") &&
             !fs::exists(home / "escape.txt"),
         "rejected paths create nothing under the destination");

  // Empty folders: a successful mkdir says so, and creates every level.
  transfer.mkdir("a/b", home.string());
  expect(json.back().find("mkdir") != std::string::npos &&
             json.back().find("true") != std::string::npos &&
             fs::is_directory(home / "a" / "b"),
         "mkdir creates the nested folder and reports success");
  transfer.mkdir("照片/空 目录/", home.string());
  expect(fs::is_directory(home / "照片" / "空 目录"),
         "mkdir accepts a trailing slash and preserves UTF-8 with spaces");
  transfer.mkdir("a/b", home.string());
  expect(json.back().find("true") != std::string::npos,
         "mkdir over an existing folder is still a success");

  const char* bad_dirs[] = {"/abs", "../up", "C:\\tmp", "\\\\server\\share",
                            "CON", "a//b"};
  for (const char* path : bad_dirs) {
    const std::size_t count = json.size();
    transfer.mkdir(path, home.string());
    expect(json.size() == count + 1 &&
               json.back().find("invalid folder path") != std::string::npos,
           "mkdir rejects an unsafe folder path");
  }

  fs::remove_all(home, ec);
}

/// A resumable upload: the first session writes a prefix and disappears, the
/// second finds the retained `.part`, continues at the host's real offset, and
/// every byte matches a one-shot transfer.
void test_upload_resume_is_byte_identical() {
  namespace fs = std::filesystem;
  const fs::path home = fresh_home("molink-upload-resume-test");
  const std::size_t size = molink::file_transfer::kChunkBytes * 2 + 137;
  std::string content(size, '\0');
  for (std::size_t index = 0; index < size; ++index) {
    content[index] = static_cast<char>((index * 37 + 11) & 0xff);
  }
  const std::size_t prefix = molink::file_transfer::kChunkBytes + 19;

  std::vector<std::string> json;
  molink::file_transfer sender(
      [&json](std::string value) { json.push_back(std::move(value)); },
      [](uint32_t, uint64_t, const uint8_t*, std::size_t) { return true; });
  sender.offer("resume.bin", size, home.string(), -1, 0, std::string(),
               "token-one");
  const uint32_t first_id = id_in(json.back());
  expect(first_id != 0, "the first resume offer is accepted");
  sender.chunk(first_id, 0,
               reinterpret_cast<const uint8_t*>(content.data()), prefix);
  sender.close_all();
  expect(count_partials(home) == 1,
         "the dropped session retains exactly one .part");

  json.clear();
  molink::file_transfer receiver(
      [&json](std::string value) { json.push_back(std::move(value)); },
      [](uint32_t, uint64_t, const uint8_t*, std::size_t) { return true; });
  receiver.offer("resume.bin", size, home.string(), -1, prefix, std::string(),
                 "token-one");
  expect(json_number(json.back(), "offset") == prefix,
         "accept reports the host's contiguous offset");
  expect(json.back().find("\"resumed\":true") != std::string::npos,
         "accept says the upload resumed");
  const uint32_t second_id = id_in(json.back());
  receiver.chunk(second_id, prefix,
                 reinterpret_cast<const uint8_t*>(content.data()) + prefix,
                 size - prefix);
  receiver.complete(second_id);
  const std::string saved = read_file(home / "resume.bin");
  expect(saved == content, "the resumed upload is byte-identical");
  expect(molink::sha256_hex(saved) ==
             molink::sha256_hex(content.data(), content.size()),
         "the resumed upload matches the one-shot SHA-256");
  expect(count_partials(home) == 0, "a completed upload clears its .part");

  // The host may already hold more than the viewer's last acknowledged
  // offset (an ack was lost). Accept must report the larger durable point.
  json.clear();
  molink::file_transfer ahead(
      [&json](std::string value) { json.push_back(std::move(value)); },
      [](uint32_t, uint64_t, const uint8_t*, std::size_t) { return true; });
  ahead.offer("ahead.bin", size, home.string(), -1, 0, std::string(),
              "token-ahead");
  const uint32_t ahead_id = id_in(json.back());
  const std::size_t durable = molink::file_transfer::kChunkBytes;
  ahead.chunk(ahead_id, 0, reinterpret_cast<const uint8_t*>(content.data()),
              durable);
  ahead.close_all();

  json.clear();
  molink::file_transfer behind(
      [&json](std::string value) { json.push_back(std::move(value)); },
      [](uint32_t, uint64_t, const uint8_t*, std::size_t) { return true; });
  behind.offer("ahead.bin", size, home.string(), -1, 100, std::string(),
               "token-ahead");
  expect(json_number(json.back(), "offset") == durable,
         "accept corrects a viewer whose offset is behind the .part");
  const uint32_t behind_id = id_in(json.back());
  behind.chunk(behind_id, durable,
               reinterpret_cast<const uint8_t*>(content.data()) + durable,
               size - durable);
  behind.complete(behind_id);
  expect(read_file(home / "ahead.bin") == content,
         "resuming from the host's offset still produces the exact file");

  // An old client sends no offset: it must always start a fresh `.part`, even
  // when a resumable one is on disk.
  json.clear();
  molink::file_transfer legacy(
      [&json](std::string value) { json.push_back(std::move(value)); },
      [](uint32_t, uint64_t, const uint8_t*, std::size_t) { return true; });
  legacy.offer("legacy.bin", 4, home.string());
  const uint32_t legacy_id = id_in(json.back());
  expect(json_number(json.back(), "offset") == 0,
         "an old client sees offset 0");
  legacy.chunk(legacy_id, 0, reinterpret_cast<const uint8_t*>("old!"), 4);
  legacy.complete(legacy_id);
  expect(read_file(home / "legacy.bin") == "old!",
         "the old client's fire-and-forget upload is unchanged");

  remove_tree(home);
}

/// The retained prefix is only resumed when the optional digest agrees.
void test_upload_resume_digest() {
  namespace fs = std::filesystem;
  const fs::path home = fresh_home("molink-upload-digest-test");
  const std::string content(70000, 'd');
  const std::size_t prefix = 40000;
  const std::string good =
      molink::sha256_hex(content.data(), prefix);
  const std::string bad = molink::sha256_hex("not the prefix", 14);

  std::vector<std::string> json;
  const auto accept_chunk = [](uint32_t, uint64_t, const uint8_t*,
                               std::size_t) { return true; };
  molink::file_transfer writer(
      [&json](std::string value) { json.push_back(std::move(value)); },
      accept_chunk);
  writer.offer("digest.bin", content.size(), home.string(), -1, 0,
               std::string(), "digest-token");
  const uint32_t id = id_in(json.back());
  writer.chunk(id, 0, reinterpret_cast<const uint8_t*>(content.data()),
               prefix);
  writer.close_all();

  json.clear();
  molink::file_transfer wrong(
      [&json](std::string value) { json.push_back(std::move(value)); },
      accept_chunk);
  wrong.offer("digest.bin", content.size(), home.string(), -1, prefix, bad,
              "digest-token");
  expect(json_number(json.back(), "offset") == 0,
         "a mismatched digest refuses to resume");
  wrong.cancel(id_in(json.back()));

  json.clear();
  molink::file_transfer right(
      [&json](std::string value) { json.push_back(std::move(value)); },
      accept_chunk);
  right.offer("digest.bin", content.size(), home.string(), -1, prefix, good,
              "digest-token");
  expect(json_number(json.back(), "offset") == prefix,
         "a matching digest resumes at the prefix");
  const uint32_t resumed = id_in(json.back());
  right.chunk(resumed, prefix,
              reinterpret_cast<const uint8_t*>(content.data()) + prefix,
              content.size() - prefix);
  right.complete(resumed);
  expect(read_file(home / "digest.bin") == content,
         "the digest-checked resume is exact");

  remove_tree(home);
}

void test_part_retention_grace() {
  namespace fs = std::filesystem;
  const fs::path home = fresh_home("molink-part-retention-test");
  const auto start = molink::file_transfer::clock::now();
  std::vector<std::string> json;
  const auto accept_chunk = [](uint32_t, uint64_t, const uint8_t*,
                               std::size_t) { return true; };
  molink::file_transfer transfer(
      [&json](std::string value) { json.push_back(std::move(value)); },
      accept_chunk);

  transfer.offer("keep.bin", 10, home.string(), -1, 0, std::string(),
                 "keep-token");
  const uint32_t id = id_in(json.back());
  transfer.chunk(id, 0, reinterpret_cast<const uint8_t*>("abcd"), 4);
  transfer.close_all();
  const fs::path part = find_partial(home, "-" + std::to_string(id) + ".part");
  expect(!part.empty() && fs::exists(part),
         "a dropped session retains its .part");
  molink::file_transfer::sweep_orphans(start + std::chrono::seconds(59));
  expect(fs::exists(part), "the .part survives inside the 60 s grace");
  // The grace starts when close_all() runs, a little after `start`, so give
  // it a comfortable margin rather than racing the registration timestamp.
  molink::file_transfer::sweep_orphans(
      start + molink::file_transfer::kResumeGrace + std::chrono::seconds(5));
  expect(!fs::exists(part), "the .part is deleted after the grace elapses");

  // A fully received upload whose `complete` never arrived is also worth
  // keeping: a reconnect can commit it without resending a byte.
  transfer.offer("whole.bin", 4, home.string(), -1, 0, std::string(),
                 "whole-token");
  const uint32_t whole_id = id_in(json.back());
  transfer.chunk(whole_id, 0, reinterpret_cast<const uint8_t*>("full"), 4);
  transfer.close_all();
  const fs::path whole =
      find_partial(home, "-" + std::to_string(whole_id) + ".part");
  expect(!whole.empty() && fs::exists(whole),
         "a fully transferred but uncommitted upload is retained");

  // An explicit cancel still deletes at once.
  transfer.offer("cancel.bin", 8, home.string(), -1, 0, std::string(),
                 "cancel-token");
  const uint32_t cancel_id = id_in(json.back());
  transfer.chunk(cancel_id, 0, reinterpret_cast<const uint8_t*>("xx"), 2);
  transfer.cancel(cancel_id);
  expect(find_partial(home, "-" + std::to_string(cancel_id) + ".part").empty(),
         "cancel removes the .part immediately");

  // Reconnect after a full transfer: accept reports size, and the existing
  // bytes are committed by `complete` with no resend.
  json.clear();
  molink::file_transfer finish(
      [&json](std::string value) { json.push_back(std::move(value)); },
      accept_chunk);
  finish.offer("whole.bin", 4, home.string(), -1, 4, std::string(),
               "whole-token");
  expect(json_number(json.back(), "offset") == 4,
         "accept reports that every byte is already present");
  finish.complete(id_in(json.back()));
  expect(read_file(home / "whole.bin") == "full",
         "complete commits the retained full upload");

  molink::file_transfer::sweep_orphans(
      molink::file_transfer::clock::now() +
      molink::file_transfer::kResumeGrace * 2);
  remove_tree(home);
}

void test_write_sandbox() {
  namespace fs = std::filesystem;
  const fs::path home = fresh_home("molink-write-sandbox-test");
  const fs::path allowed = home / "allowed";
  const fs::path outside = home / "outside";
  std::error_code ec;
  fs::create_directories(allowed, ec);
  fs::create_directories(outside, ec);
  {
    std::ofstream secret(outside / "secret.txt", std::ios::binary);
    secret << "nope";
  }

  std::vector<std::string> json;
  const auto accept_chunk = [](uint32_t, uint64_t, const uint8_t*,
                               std::size_t) { return true; };
  molink::file_transfer transfer(
      [&json](std::string value) { json.push_back(std::move(value)); },
      accept_chunk);
  transfer.set_write_policy([&allowed]() {
    molink::file_transfer::write_policy policy;
    policy.enabled = true;
    policy.roots.push_back(allowed.string());
    return policy;
  });

  transfer.offer("inside.txt", 2, allowed.string());
  expect(json.back().find("\"accept\"") != std::string::npos,
         "an upload inside the sandbox root is accepted");
  const uint32_t inside_id = id_in(json.back());
  transfer.chunk(inside_id, 0, reinterpret_cast<const uint8_t*>("ok"), 2);
  transfer.complete(inside_id);
  expect(fs::exists(allowed / "inside.txt"),
         "the in-root upload lands on disk");

  json.clear();
  transfer.offer("escape.txt", 2, outside.string());
  expect(json.back().find("\"action\":\"forbidden_path\"") !=
             std::string::npos &&
             json.back().find("\"ok\":false") != std::string::npos &&
             json.back().find(allowed.string()) != std::string::npos,
         "an upload outside the root answers forbidden_path with the root");
  expect(!fs::exists(outside / "escape.txt"),
         "a forbidden upload creates nothing");

  json.clear();
  transfer.mkdir("nested", outside.string());
  expect(json.back().find("\"action\":\"forbidden_path\"") !=
             std::string::npos &&
             !fs::exists(outside / "nested"),
         "mkdir outside the root is refused before it creates anything");
  transfer.mkdir("nested", allowed.string());
  expect(json.back().find("\"ok\":true") != std::string::npos &&
             fs::is_directory(allowed / "nested"),
         "mkdir inside the root still works");

  json.clear();
  transfer.request((outside / "secret.txt").string());
  expect(json.back().find("\"action\":\"forbidden_path\"") !=
             std::string::npos,
         "a download source outside the root is refused when sandboxed");

  // Off by default: the same destination is accepted.
  json.clear();
  molink::file_transfer open(
      [&json](std::string value) { json.push_back(std::move(value)); },
      accept_chunk);
  open.set_write_policy([]() { return molink::file_transfer::write_policy{}; });
  open.offer("free.txt", 2, outside.string());
  expect(json.back().find("\"accept\"") != std::string::npos,
         "the sandbox is off by default");

  // Enabled with no configured root falls back to ~/moLink.
  json.clear();
  molink::file_transfer fallback(
      [&json](std::string value) { json.push_back(std::move(value)); },
      accept_chunk);
  fallback.set_write_policy([]() {
    molink::file_transfer::write_policy policy;
    policy.enabled = true;
    return policy;
  });
  fallback.offer("fallback.txt", 2, outside.string());
  expect(json.back().find("\"action\":\"forbidden_path\"") !=
             std::string::npos,
         "an enabled sandbox without roots defaults to ~/moLink");
  fallback.offer("fallback.txt", 2, std::string());
  expect(json.back().find("\"accept\"") != std::string::npos,
         "the default receive folder is inside the default root");

  fs::remove_all(home, ec);
}

void test_download_pull_offset() {
  namespace fs = std::filesystem;
  const fs::path home = fresh_home("molink-download-offset-test");
  const std::size_t size = molink::file_transfer::kChunkBytes * 2 + 77;
  std::string content(size, '\0');
  for (std::size_t index = 0; index < size; ++index) {
    content[index] = static_cast<char>((index * 13 + 5) & 0xff);
  }
  {
    std::ofstream out(home / "source.bin", std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
  }
  std::vector<std::string> json;
  std::vector<std::pair<uint64_t, std::string>> chunks;
  molink::file_transfer transfer(
      [&json](std::string value) { json.push_back(std::move(value)); },
      [&chunks](uint32_t, uint64_t offset, const uint8_t* data,
                std::size_t len) {
        chunks.emplace_back(
            offset, std::string(reinterpret_cast<const char*>(data), len));
        return true;
      });

  transfer.request("source.bin");
  expect(json.back().find("\"action\":\"sending\"") != std::string::npos,
         "request announces the download");
  const uint32_t id = id_in(json.back());
  const uint64_t resume = molink::file_transfer::kChunkBytes + 31;
  transfer.pull(id, resume);
  expect(!chunks.empty() && chunks.back().first == resume,
         "pull with an offset seeks the host file");
  // Keep that first resumed chunk: it is real data after the saved prefix,
  // and dropping it would leave a hole in the reconstruction below.
  while (transfer.has_transfers()) {
    transfer.pull(id);
  }
  std::string received(size, '\0');
  for (const auto& chunk : chunks) {
    if (chunk.first >= size) continue;
    // `size` is 64-bit and `size()` is size_t, which is 32-bit on the Windows
    // 7 x86 build; min() needs both sides to be the same type.
    const std::uint64_t take =
        (std::min)(static_cast<std::uint64_t>(chunk.second.size()),
                   size - chunk.first);
    received.replace(chunk.first, static_cast<std::size_t>(take),
                     chunk.second.data(), static_cast<std::size_t>(take));
  }
  // The prefix was already saved before the reconnect.
  received.replace(0, static_cast<std::size_t>(resume),
                   content.data(), static_cast<std::size_t>(resume));
  expect(received == content,
         "a download resumed at an offset reconstructs the exact file");
  expect(molink::sha256_hex(received) ==
             molink::sha256_hex(content.data(), content.size()),
         "the resumed download matches the source SHA-256");

  // An offset at or past EOF ends the transfer immediately.
  json.clear();
  chunks.clear();
  transfer.request("source.bin");
  const uint32_t eof_id = id_in(json.back());
  transfer.pull(eof_id, size);
  expect(!transfer.has_transfers() &&
             json.back().find("\"action\":\"eof\"") != std::string::npos,
         "pulling at the file size answers eof without reading");

  remove_tree(home);
}

}  // namespace

int main() {
  test_download_retries_same_offset_after_backpressure();
  test_download_and_upload_slots_are_reclaimed();
  test_upload_paths_and_mkdir();
  test_upload_resume_is_byte_identical();
  test_upload_resume_digest();
  test_part_retention_grace();
  test_write_sandbox();
  test_download_pull_offset();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
