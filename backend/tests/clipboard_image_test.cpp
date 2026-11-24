// Clipboard image rules that do not touch a platform clipboard: the 8 MiB
// compressed cap, the de-duplication hash and the owner decision that keeps a
// self-written image from being announced as a local copy.
#include <cstdio>
#include <vector>

#include "clipboard/clipboard_image.h"

namespace {

using namespace molink;

int checks = 0;
int failures = 0;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

void test_limit_is_inclusive_and_clears_the_bytes() {
  std::vector<uint8_t> at_limit(kClipboardImageMaxBytes, 0x42);
  clipboard_image image = clipboard_image_clamped(std::move(at_limit));
  expect(!image.too_large, "exactly 8 MiB is allowed");
  expect(!image.empty(), "the allowed image keeps its bytes");
  expect(image.source_bytes == kClipboardImageMaxBytes,
         "the source size is recorded for the log");

  std::vector<uint8_t> over(kClipboardImageMaxBytes + 1, 0x42);
  clipboard_image rejected = clipboard_image_clamped(std::move(over));
  expect(rejected.too_large, "one byte over the cap reports too_large");
  expect(rejected.empty(), "the oversized bytes are dropped");
  expect(rejected.source_bytes == kClipboardImageMaxBytes + 1,
         "the log still names the real size");
}

void test_hash_distinguishes_images() {
  const std::vector<uint8_t> first = {1, 2, 3, 4};
  const std::vector<uint8_t> second = {1, 2, 3, 5};
  expect(!clipboard_image_hash(first).empty(), "a hash is produced");
  expect(clipboard_image_hash(first) == clipboard_image_hash(first),
         "the same bytes hash the same");
  expect(clipboard_image_hash(first) != clipboard_image_hash(second),
         "different bytes hash differently");
  expect(clipboard_image_hash(first).size() == 16,
         "the hash is 16 lower-case hex characters");
}

void test_self_written_image_is_not_announced() {
  clipboard_image valid;
  valid.png = {0x89, 'P', 'N', 'G'};
  valid.source_bytes = valid.png.size();

  expect(clipboard_image_disposition_for(clipboard_owner::self, valid) ==
             clipboard_image_disposition::drop_self,
         "our own image write is an echo, not a local copy");
  expect(clipboard_image_disposition_for(clipboard_owner::external, valid) ==
             clipboard_image_disposition::announce,
         "an external copy is announced");
  // A platform that cannot name the owner must still sync: dropping it would
  // lose every screenshot on that desktop.
  expect(clipboard_image_disposition_for(clipboard_owner::unknown, valid) ==
             clipboard_image_disposition::announce,
         "an unknown owner is accepted");

  clipboard_image none;
  expect(clipboard_image_disposition_for(clipboard_owner::external, none) ==
             clipboard_image_disposition::drop_empty,
         "an empty clipboard is not announced");

  clipboard_image too_large;
  too_large.too_large = true;
  too_large.source_bytes = kClipboardImageMaxBytes + 1;
  expect(clipboard_image_disposition_for(clipboard_owner::external,
                                         too_large) ==
             clipboard_image_disposition::drop_too_large,
         "an over-limit image is reported separately from an empty one");
  expect(clipboard_image_disposition_for(clipboard_owner::self, too_large) ==
             clipboard_image_disposition::drop_self,
         "the self check wins over the size check");
}

}  // namespace

int main() {
  test_limit_is_inclusive_and_clears_the_bytes();
  test_hash_distinguishes_images();
  test_self_written_image_is_not_announced();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
