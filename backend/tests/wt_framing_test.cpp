// Datagram framing for the WebTransport transport: a video frame is cut into
// MTU-sized datagrams and reassembled by frame id.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "transport/wt_framing.h"

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

std::vector<uint8_t> frame_bytes(std::size_t len) {
  std::vector<uint8_t> bytes(len);
  for (std::size_t i = 0; i < len; ++i) {
    bytes[i] = static_cast<uint8_t>((i * 31 + 7) & 0xff);
  }
  return bytes;
}

void test_fragmentation_shape() {
  const auto small = frame_bytes(100);
  const auto one = molink::fragment_frame(molink::datagram_channel::video, small.data(), small.size(), 1);
  expect(one.size() == 1, "a small frame is one datagram");

  const auto big = frame_bytes(5000);
  const auto many = molink::fragment_frame(molink::datagram_channel::video, big.data(), big.size(), 2);
  expect(many.size() >= 5, "a 5 KiB frame is several datagrams");
  bool sized = true;
  for (const auto& fragment : many) {
    if (fragment.bytes.size() > molink::kDatagramPayloadBytes) sized = false;
  }
  expect(sized, "no datagram exceeds the payload budget");

  const auto huge = frame_bytes(molink::kDatagramFrameLimit + 1);
  expect(molink::fragment_frame(molink::datagram_channel::video, huge.data(), huge.size(), 3).empty(),
         "a frame past the limit is not fragmented");
  expect(molink::fragment_frame(molink::datagram_channel::video, nullptr, 0, 4).empty(),
         "an empty frame is not fragmented");
}

void test_reassembly_in_order() {
  const auto frame = frame_bytes(4000);
  const auto fragments = molink::fragment_frame(molink::datagram_channel::video, frame.data(), frame.size(), 7);
  molink::datagram_reassembler assembler;
  std::vector<uint8_t> out;
  int completions = 0;
  for (const auto& fragment : fragments) {
    if (assembler.push(fragment.bytes.data(), fragment.bytes.size(), out)) {
      ++completions;
    }
  }
  expect(completions == 1, "the frame completes exactly once");
  expect(out == frame, "the reassembled frame matches byte for byte");
}

void test_reassembly_out_of_order() {
  const auto frame = frame_bytes(3000);
  auto fragments = molink::fragment_frame(molink::datagram_channel::video, frame.data(), frame.size(), 9);
  molink::datagram_reassembler assembler;
  std::vector<uint8_t> out;
  bool complete = false;
  // Deliver back to front: the assembler keys on the fragment index, so order
  // does not matter.
  for (auto it = fragments.rbegin(); it != fragments.rend(); ++it) {
    if (assembler.push(it->bytes.data(), it->bytes.size(), out)) complete = true;
  }
  expect(complete, "out-of-order fragments still complete the frame");
  expect(out == frame, "and reassemble correctly");
}

void test_missing_fragment_drops_frame() {
  const auto frame = frame_bytes(4000);
  const auto fragments = molink::fragment_frame(molink::datagram_channel::video, frame.data(), frame.size(), 11);
  molink::datagram_reassembler assembler;
  std::vector<uint8_t> out;
  bool complete = false;
  for (std::size_t i = 0; i < fragments.size(); ++i) {
    if (i == 1) continue;  // one lost on the wire
    if (assembler.push(fragments[i].bytes.data(), fragments[i].bytes.size(),
                       out)) {
      complete = true;
    }
  }
  expect(!complete, "a frame with a lost fragment never completes");

  // The next frame must still reassemble: the partial one is discarded.
  const auto next = frame_bytes(1200);
  const auto next_fragments = molink::fragment_frame(molink::datagram_channel::video, next.data(), next.size(), 12);
  bool next_complete = false;
  for (const auto& fragment : next_fragments) {
    if (assembler.push(fragment.bytes.data(), fragment.bytes.size(), out)) {
      next_complete = true;
    }
  }
  expect(next_complete && out == next,
         "the next frame reassembles after a lost one");
}

void test_new_frame_supersedes_partial() {
  const auto first = frame_bytes(4000);
  const auto second = frame_bytes(2000);
  const auto first_fragments =
      molink::fragment_frame(molink::datagram_channel::video, first.data(), first.size(), 21);
  const auto second_fragments =
      molink::fragment_frame(molink::datagram_channel::video, second.data(), second.size(), 22);
  molink::datagram_reassembler assembler;
  std::vector<uint8_t> out;
  // First frame loses its tail; the second frame arrives whole.
  for (std::size_t i = 0; i + 1 < first_fragments.size(); ++i) {
    assembler.push(first_fragments[i].bytes.data(),
                   first_fragments[i].bytes.size(), out);
  }
  bool complete = false;
  for (const auto& fragment : second_fragments) {
    if (assembler.push(fragment.bytes.data(), fragment.bytes.size(), out)) {
      complete = true;
    }
  }
  expect(complete && out == second,
         "a newer frame supersedes an incomplete older one");
  expect(assembler.dropped() >= 1, "the dropped partial frame is counted");
}

void test_channels_do_not_interfere() {
  // An audio datagram that lands in the middle of a video frame must not tear
  // either reassembly apart.
  const auto video = frame_bytes(3000);
  const auto audio = frame_bytes(400);
  const auto video_parts = molink::fragment_frame(
      molink::datagram_channel::video, video.data(), video.size(), 41);
  const auto audio_parts = molink::fragment_frame(
      molink::datagram_channel::audio, audio.data(), audio.size(), 42);
  expect(video_parts.size() >= 2 && audio_parts.size() >= 1,
         "both channels produce fragments");

  molink::datagram_reassembler video_rx;
  molink::datagram_reassembler audio_rx;
  std::vector<uint8_t> out;
  bool video_done = false;
  bool audio_done = false;
  // Interleave: video fragment 0, all audio, then the rest of the video.
  video_done |= video_rx.push(video_parts[0].bytes.data(),
                              video_parts[0].bytes.size(), out);
  for (const auto& part : audio_parts) {
    const auto tag = molink::datagram_channel_of(part.bytes.data(),
                                                 part.bytes.size());
    expect(tag == molink::datagram_channel::audio,
           "an audio fragment is tagged audio");
    audio_done |= audio_rx.push(part.bytes.data(), part.bytes.size(), out);
  }
  std::vector<uint8_t> video_out;
  for (std::size_t i = 1; i < video_parts.size(); ++i) {
    if (video_rx.push(video_parts[i].bytes.data(),
                      video_parts[i].bytes.size(), video_out)) {
      video_done = true;
    }
  }
  expect(audio_done, "the audio frame completes between video fragments");
  expect(video_done && video_out == video,
         "the video frame still completes intact");
}

void test_malformed_input() {
  molink::datagram_reassembler assembler;
  std::vector<uint8_t> out;
  const uint8_t tiny[4] = {0, 0, 0, 0};
  expect(!assembler.push(tiny, sizeof(tiny), out),
         "a datagram without a fragment header is ignored");
  expect(!assembler.push(nullptr, 0, out), "an empty datagram is ignored");

  const auto frame = frame_bytes(500);
  auto fragments = molink::fragment_frame(molink::datagram_channel::video, frame.data(), frame.size(), 31);
  expect(!fragments.empty(), "the sample frame fragments");
  // A corrupted count must not complete anything.
  fragments[0].bytes[6] = 0xff;
  fragments[0].bytes[7] = 0xff;
  expect(!assembler.push(fragments[0].bytes.data(), fragments[0].bytes.size(),
                         out),
         "a fragment with a stale count is ignored");
}

}  // namespace

int main() {
  test_fragmentation_shape();
  test_reassembly_in_order();
  test_reassembly_out_of_order();
  test_missing_fragment_drops_frame();
  test_new_frame_supersedes_partial();
  test_channels_do_not_interfere();
  test_malformed_input();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
