#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "clipboard/clipboard.h"
#include "clipboard_stage.h"

namespace {

namespace fs = std::filesystem;
using molink::clipboard_stage;
using molink::stage_error;

int checks = 0;
int failures = 0;

void expect(bool value, const char* message) {
  ++checks;
  if (!value) {
    ++failures;
    std::printf("FAIL: %s\n", message);
  }
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

class fake_clipboard : public molink::clipboard_service {
 public:
  bool available() const override { return true; }
  std::string backend() const override { return "fake"; }
  void set_text(std::string) override {}
  void request_text(text_callback) override {}
  bool set_files(const std::vector<std::string>& paths) override {
    ++calls;
    applied = paths;
    return accept;
  }

  int calls = 0;
  bool accept = true;
  std::vector<std::string> applied;
};

void test_sanitize_name() {
  expect(clipboard_stage::sanitize_name("a.txt") == "a.txt",
         "a plain name is kept");
  expect(clipboard_stage::sanitize_name("../evil") == std::string(),
         "parent traversal is rejected");
  expect(clipboard_stage::sanitize_name("a/b") == std::string(),
         "a slash is rejected");
  expect(clipboard_stage::sanitize_name("a\\b") == std::string(),
         "a backslash is rejected");
  expect(clipboard_stage::sanitize_name("..") == std::string(),
         "dot-dot is rejected");
  expect(clipboard_stage::sanitize_name("   ") == std::string(),
         "whitespace only is rejected");
  expect(clipboard_stage::sanitize_name("a\nb\x7f.txt") == "ab.txt",
         "control characters are removed");
  expect(clipboard_stage::sanitize_name(" a:b ") == std::string(),
         "a drive/stream colon is rejected");
  const std::string long_name(300, 'x');
  expect(clipboard_stage::sanitize_name(long_name).size() <= 180,
         "an over-long name is bounded");
}

void test_append_and_apply() {
  const fs::path dir = fs::temp_directory_path() / "molink-clipboard-stage-a";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  uint64_t clock = 1'000'000;
  fake_clipboard clipboard;
  clipboard_stage stage(
      dir.string(),
      [&clipboard](const std::vector<std::string>& paths) {
        return clipboard.set_files(paths);
      },
      [&clock] { return clock; });

  std::string id;
  std::string path;
  expect(stage.begin("a.txt", 5, id, path) == stage_error::none,
         "begin accepts a normal file");
  expect(!id.empty(), "begin returns an opaque id");
  expect(fs::path(path).filename() == "a.txt", "the final name is kept");
  expect(fs::exists(fs::path(dir) / (".stage-" + id)),
         "bytes go to a private temp file first");

  uint64_t received = 0;
  expect(stage.append(id, 0, "he", 2, received) == stage_error::none,
         "the first chunk appends");
  expect(received == 2, "the received count advances");
  expect(stage.append(id, 2, "llo", 3, received) == stage_error::none,
         "the next contiguous chunk appends");
  expect(received == 5, "the received count reaches the declared size");
  expect(stage.append(id, 5, "x", 1, received) == stage_error::too_large,
         "a chunk beyond the declared size is refused");
  expect(stage.append(id, 4, "x", 1, received) == stage_error::conflict,
         "a non-contiguous offset is refused");

  std::size_t count = 0;
  expect(stage.apply({id}, count) == stage_error::none, "apply succeeds");
  expect(count == 1, "apply reports the applied count");
  expect(clipboard.calls == 1 && clipboard.applied.size() == 1,
         "apply calls the clipboard backend once");
  expect(!clipboard.applied.empty() && clipboard.applied[0] == path,
         "the clipboard receives the final path");
  expect(read_file(path) == "hello", "the applied file has all bytes");
  expect(stage.pending() == 0, "the entry is consumed by apply");
  expect(stage.apply({id}, count) == stage_error::not_found,
         "a consumed id cannot be applied twice");
  expect(fs::exists(path), "the applied file survives for the clipboard");

  fs::remove_all(dir, ec);
}

void test_offset_gap_and_incomplete_apply() {
  const fs::path dir = fs::temp_directory_path() / "molink-clipboard-stage-b";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  uint64_t clock = 1'000'000;
  fake_clipboard clipboard;
  clipboard_stage stage(
      dir.string(),
      [&clipboard](const std::vector<std::string>& paths) {
        return clipboard.set_files(paths);
      },
      [&clock] { return clock; });

  std::string id;
  std::string path;
  expect(stage.begin("b.bin", 4, id, path) == stage_error::none,
         "begin accepts a second file");
  uint64_t received = 0;
  expect(stage.append(id, 1, "x", 1, received) == stage_error::conflict,
         "starting at a later offset is refused");
  expect(stage.append(id, 0, "abcd", 4, received) == stage_error::none,
         "a whole chunk appends");
  std::size_t count = 0;
  expect(stage.apply({id}, count) == stage_error::none,
         "a complete file applies");

  std::string partial_id;
  std::string partial_path;
  expect(stage.begin("c.bin", 4, partial_id, partial_path) ==
             stage_error::none,
         "begin accepts an incomplete file");
  expect(stage.append(partial_id, 0, "ab", 2, received) == stage_error::none,
         "a partial file can receive bytes");
  expect(stage.apply({partial_id}, count) == stage_error::conflict,
         "apply refuses an incomplete entry");
  expect(clipboard.calls == 1, "an incomplete batch never reaches the backend");

  fs::remove_all(dir, ec);
}

void test_same_name_numbering() {
  const fs::path dir = fs::temp_directory_path() / "molink-clipboard-stage-c";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  clipboard_stage stage(dir.string(), [](const std::vector<std::string>&) {
    return true;
  });
  std::string first_id;
  std::string first_path;
  std::string second_id;
  std::string second_path;
  expect(stage.begin("same.txt", 0, first_id, first_path) == stage_error::none,
         "the first same.txt is accepted");
  expect(stage.begin("same.txt", 0, second_id, second_path) ==
             stage_error::none,
         "the second same.txt is accepted");
  expect(first_path != second_path, "same names get distinct final paths");
  expect(second_path.find("(1)") != std::string::npos,
         "the duplicate is numbered");

  fs::remove_all(dir, ec);
}

void test_limits_and_bad_names() {
  const fs::path dir = fs::temp_directory_path() / "molink-clipboard-stage-d";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  clipboard_stage stage(dir.string(), [](const std::vector<std::string>&) {
    return true;
  });
  std::string id;
  std::string path;
  expect(stage.begin("big.bin", clipboard_stage::kMaxFileBytes + 1, id, path) ==
             stage_error::too_large,
         "a file above the staging cap is refused");
  expect(stage.begin("../escape", 1, id, path) == stage_error::bad_request,
         "a traversal name is a bad request");
  expect(stage.begin("a/b", 1, id, path) == stage_error::bad_request,
         "a separated name is a bad request");
  expect(stage.begin("..", 1, id, path) == stage_error::bad_request,
         "dot-dot is a bad request");

  std::size_t count = 0;
  expect(stage.apply({}, count) == stage_error::bad_request,
         "an empty apply is a bad request");
  expect(stage.apply({"missing"}, count) == stage_error::not_found,
         "an unknown id is not found");

  fs::remove_all(dir, ec);
}

void test_expiry_is_lazy() {
  const fs::path dir = fs::temp_directory_path() / "molink-clipboard-stage-e";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  uint64_t clock = 5'000;
  clipboard_stage stage(dir.string(), [](const std::vector<std::string>&) {
    return true;
  }, [&clock] { return clock; });
  std::string id;
  std::string path;
  expect(stage.begin("old.bin", 4, id, path) == stage_error::none,
         "begin creates the entry");
  const fs::path temp = fs::path(dir) / (".stage-" + id);
  expect(fs::exists(temp), "the temp file exists before expiry");

  clock += clipboard_stage::kEntryTtlMs + 1;
  uint64_t received = 0;
  expect(stage.append(id, 0, "ab", 2, received) == stage_error::not_found,
         "an expired entry is dropped on the next request");
  expect(stage.pending() == 0, "the expired entry is gone");
  expect(!fs::exists(temp), "the expired partial file is removed");

  fs::remove_all(dir, ec);
}

void test_apply_failure_can_retry() {
  const fs::path dir = fs::temp_directory_path() / "molink-clipboard-stage-f";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  fake_clipboard clipboard;
  clipboard.accept = false;
  clipboard_stage stage(
      dir.string(),
      [&clipboard](const std::vector<std::string>& paths) {
        return clipboard.set_files(paths);
      });
  std::string id;
  std::string path;
  expect(stage.begin("r.txt", 2, id, path) == stage_error::none,
         "begin accepts the retry file");
  uint64_t received = 0;
  expect(stage.append(id, 0, "ok", 2, received) == stage_error::none,
         "the retry file is complete");
  std::size_t count = 0;
  expect(stage.apply({id}, count) == stage_error::apply_failed,
         "a refusing backend surfaces apply_failed");
  expect(clipboard.calls == 1, "the backend was called");
  expect(stage.pending() == 1, "a failed apply keeps the entry for a retry");

  clipboard.accept = true;
  expect(stage.apply({id}, count) == stage_error::none,
         "a retry succeeds without re-renaming");
  expect(clipboard.calls == 2, "the backend is called again");
  expect(read_file(path) == "ok", "the retried file is intact");

  fs::remove_all(dir, ec);
}

void test_batch_directory() {
  const fs::path dir = fs::temp_directory_path() / "molink-clipboard-stage-g";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  clipboard_stage stage(dir.string(), [](const std::vector<std::string>&) {
    return true;
  });
  std::string id;
  std::string path;
  const std::string batch = "0123abcd0123abcd0123abcd0123abcd0123abcd";
  expect(stage.begin("a.txt", 1, id, path, batch) == stage_error::none,
         "a SHA-1 batch directory is accepted");
  expect(fs::path(path).parent_path().filename() == batch,
         "the final file lives in the batch directory");
  expect(fs::is_directory(fs::path(dir) / batch),
         "the batch directory exists");

  // The same name in a later batch is not renamed with " (1)".
  std::string second_id;
  std::string second_path;
  const std::string batch2 = "ffffffffffffffffffffffffffffffffffffffff";
  expect(stage.begin("a.txt", 1, second_id, second_path, batch2) ==
             stage_error::none,
         "the same name in a new batch is accepted");
  expect(fs::path(second_path).filename() == "a.txt",
         "a new batch keeps the original name");

  expect(stage.begin("b.txt", 1, id, path, "../escape") ==
             stage_error::bad_request,
         "a path-like batch is refused");
  expect(stage.begin("b.txt", 1, id, path, "xyz") ==
             stage_error::bad_request,
         "a short/non-hex batch is refused");

  fs::remove_all(dir, ec);
}

void test_no_config_directory() {
  clipboard_stage stage(std::string(), [](const std::vector<std::string>&) {
    return true;
  });
  std::string id;
  std::string path;
  expect(stage.begin("a.txt", 1, id, path) == stage_error::unavailable,
         "an empty directory makes staging unavailable");
  expect(!stage.available(), "available() reports the same");
}

}  // namespace

void test_abort_removes_temp() {
  const fs::path dir = fs::temp_directory_path() / "molink-clipboard-stage-abort";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  clipboard_stage stage(dir.string(), [](const std::vector<std::string>&) {
    return true;
  });
  std::string id;
  std::string path;
  expect(stage.begin("half.bin", 10, id, path) == stage_error::none,
         "the partial entry is accepted");
  uint64_t received = 0;
  expect(stage.append(id, 0, "12345", 5, received) == stage_error::none,
         "the partial bytes are accepted");
  stage.abort(id);
  expect(stage.pending() == 0, "abort drops the staging entry");

  bool has_file = false;
  for (const auto& entry : fs::directory_iterator(dir)) {
    (void)entry;
    has_file = true;
  }
  expect(!has_file, "abort deletes the half-written file");

  std::size_t count = 0;
  expect(stage.apply({id}, count) == stage_error::not_found,
         "an aborted id can no longer be applied");

  fs::remove_all(dir, ec);
}

int main() {
  test_sanitize_name();
  test_append_and_apply();
  test_offset_gap_and_incomplete_apply();
  test_same_name_numbering();
  test_limits_and_bad_names();
  test_expiry_is_lazy();
  test_apply_failure_can_retry();
  test_batch_directory();
  test_no_config_directory();
  test_abort_removes_temp();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
