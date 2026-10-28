#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace molink {

/// Publishes decoded pictures to the desktop shell so it can render them
/// natively, outside the WebView.
///
/// The core is the only writer and the shell the only reader, so the file is
/// a plain double buffer with a sequence number in the header: the writer
/// fills the slot named by the parity of the next sequence and then publishes
/// that sequence with a release store; a reader that sees sequence N reads
/// slot N & 1 and never blocks.
///
/// Format values match `surface_format` below. The mapping lives in /dev/shm
/// on Linux and next to the configuration on Windows, and is created lazily
/// when MOLINK_SURFACE is set or the native shell asks for it.
class frame_surface {
 public:
  enum class format : uint32_t {
    bgra = 1,
    rgb565 = 2,
  };

  static frame_surface& instance();

  /// Turns the surface on and returns the mapped path (empty on failure).
  /// `max_width`/`max_height` size the two slots up front so the file never
  /// has to be re-mapped while a session runs.
  std::string enable(int max_width, int max_height);

  /// True once the surface is enabled.
  bool active() const;

  /// Path of the mapped file, for the shell to open.
  std::string path() const;

  /// Publishes one picture. A no-op when the surface is not active.
  void publish(const uint8_t* pixels, int width, int height, int stride,
               format pixel_format);

  /// Sequence of the newest published picture (0 = nothing yet).
  uint64_t sequence() const;

 private:
  frame_surface() = default;
  ~frame_surface();
  frame_surface(const frame_surface&) = delete;
  frame_surface& operator=(const frame_surface&) = delete;

  bool map(int max_width, int max_height);
  void unmap();

  int fd_ = -1;
  void* mapping_ = nullptr;
  std::size_t bytes_ = 0;
  std::size_t slot_bytes_ = 0;
  int max_width_ = 0;
  int max_height_ = 0;
  std::string path_;
  uint64_t sequence_ = 0;
};

/// Allocates the surface when MOLINK_SURFACE is set; safe to call more than
/// once. Returns true when a surface is available.
bool init_frame_surface_from_env();

}  // namespace molink
