// GPU-to-CPU hand-over for the AV1 stream encoder. The failure path is the
// part that only shows up on a machine with a flaky driver, so it is exercised
// here with fake encoders instead.
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "capture/av1_stream_fallback.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void expect(bool ok, const char* what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

/// Records what it was asked to encode and answers with one byte.
class fake_encoder final : public molink::av1_stream_encoder {
 public:
  fake_encoder(std::string label, int failures_before_success)
      : label_(std::move(label)), failures_left_(failures_before_success) {}

  bool encode_stream(const uint8_t*, std::size_t, int, int, int qp,
                     bool keyframe, std::vector<uint8_t>& out,
                     bool& out_keyframe, std::string& error) override {
    ++calls;
    last_keyframe = keyframe;
    last_qp = qp;
    if (failures_left_ > 0) {
      --failures_left_;
      error = label_ + ": simulated failure";
      return false;
    }
    out.assign(1, static_cast<uint8_t>(label_[0]));
    out_keyframe = keyframe;
    return true;
  }

  int calls = 0;
  bool last_keyframe = false;
  int last_qp = 0;

 private:
  std::string label_;
  int failures_left_;
};

struct fixture {
  // The fixture keeps its own reference: when the fallback drops the GPU it
  // releases the only other one, and the fake must stay alive to be inspected.
  std::shared_ptr<fake_encoder> gpu;
  std::shared_ptr<fake_encoder> cpu;
  std::unique_ptr<molink::fallback_av1_stream_encoder> encoder;
  int cpu_created = 0;

  explicit fixture(int gpu_failures) {
    gpu = std::make_shared<fake_encoder>("gpu", gpu_failures);
    encoder = std::make_unique<molink::fallback_av1_stream_encoder>(
        gpu, [this] {
          ++cpu_created;
          cpu = std::make_shared<fake_encoder>("cpu", 0);
          return std::shared_ptr<molink::av1_stream_encoder>(cpu);
        });
  }

  bool encode(bool keyframe, std::vector<uint8_t>& out, bool& out_keyframe,
              std::string& error) {
    const uint8_t pixel = 0;
    return encoder->encode_stream(&pixel, 4, 1, 1, 30, keyframe, out,
                                  out_keyframe, error);
  }
};

void test_gpu_only() {
  fixture f(0);
  std::vector<uint8_t> out;
  bool keyframe = false;
  std::string error;
  expect(f.encode(false, out, keyframe, error), "the GPU frame encodes");
  expect(f.cpu_created == 0 && !f.encoder->has_cpu(),
         "a healthy GPU session never builds the CPU encoder");
  expect(!keyframe && out.size() == 1 && out[0] == 'g',
         "the GPU packet comes back unchanged");
  expect(f.gpu->calls == 1, "the GPU saw the frame");
}

void test_failure_reencodes_on_cpu() {
  fixture f(1);
  std::vector<uint8_t> out;
  bool keyframe = false;
  std::string error;
  // The first call fails on the GPU and must still deliver a picture.
  expect(f.encode(false, out, keyframe, error),
         "a GPU failure still produces a frame");
  expect(f.cpu_created == 1, "the CPU encoder is created on the first failure");
  expect(f.cpu != nullptr && f.cpu->calls == 1, "the CPU encoded that frame");
  expect(f.cpu->last_keyframe,
         "the first CPU frame is a keyframe even if the caller sent a delta");
  expect(keyframe, "the delivered frame is reported as a keyframe");
  expect(out.size() == 1 && out[0] == 'c', "the CPU packet is what came back");
  expect(f.gpu->calls == 1, "the GPU was tried first");
}

void test_cpu_keyframe_cleared_after_first_frame() {
  // The GPU keeps failing, so the second frame is also encoded on the CPU.
  fixture f(100);
  std::vector<uint8_t> out;
  bool keyframe = false;
  std::string error;
  f.encode(false, out, keyframe, error);
  expect(f.cpu->last_keyframe, "the resync frame is a keyframe");
  expect(f.encode(false, out, keyframe, error), "the next frame encodes");
  expect(!f.cpu->last_keyframe,
         "later CPU frames follow the caller's keyframe request");
  expect(!keyframe, "and are reported as deltas");
}

void test_gpu_dropped_after_repeated_failures() {
  fixture f(100);
  std::vector<uint8_t> out;
  bool keyframe = false;
  std::string error;
  constexpr int kLimit = molink::fallback_av1_stream_encoder::kGpuFailureLimit;
  for (int i = 0; i < kLimit; ++i) {
    expect(f.encode(false, out, keyframe, error),
           "every failing GPU frame is still delivered by the CPU");
    expect(f.encoder->has_gpu() == (i + 1 < kLimit),
           "the GPU stays in the chain until the failure limit");
  }
  expect(f.gpu->calls == kLimit, "the GPU was tried once per frame");
  expect(f.encode(false, out, keyframe, error), "the next frame still encodes");
  expect(!f.encoder->has_gpu(), "the GPU is dropped after the failure limit");
  expect(f.gpu->calls == kLimit, "the dropped GPU is not tried again");
  expect(f.cpu->calls == kLimit + 1,
         "the CPU carries every frame after the drop");
}

void test_qp_is_passed_through() {
  fixture f(0);
  std::vector<uint8_t> out;
  bool keyframe = false;
  std::string error;
  f.encode(false, out, keyframe, error);
  expect(f.gpu->last_qp == 30, "the caller's quantiser reaches the encoder");
}

}  // namespace

int main() {
  test_gpu_only();
  test_failure_reencodes_on_cpu();
  test_cpu_keyframe_cleared_after_first_frame();
  test_gpu_dropped_after_repeated_failures();
  test_qp_is_passed_through();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
