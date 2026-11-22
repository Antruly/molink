#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "clipboard_lazy.h"

namespace {

using molink::lazy_clipboard;
using molink::lazy_clipboard_file;
using molink::lazy_file_source;
using molink::lazy_offer_error;
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

}  // namespace

int main() {
  test_offer_limits();
  test_registry_lifecycle();
  test_source_completion();
  test_total_timeout();
  test_chunk_timeout();
  test_session_loss_wakes_waiter();
  test_ensure_all_shares_deadline();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
