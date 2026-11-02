// Session wiring for the native shell: connects the WS client to the local
// core, sends hello, and feeds the toolbar's session_stats from the traffic.
#pragma once

#include <atomic>
#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ui_kit.h"
#include "ws_client.h"

namespace molink::net {

/// One `file action=listing` entry (name is a single path segment).
struct file_listing_entry {
  std::wstring name;
  bool dir = false;
  uint64_t size = 0;
  uint64_t mtime = 0;
};

/// File-protocol message surfaced to the shell.
struct file_event {
  std::wstring action;  ///< listing/sending/eof/done/reject/cancel/error
                        ///< (the shell also synthesizes `downloaded`)
  uint32_t id = 0;
  uint32_t chunk = 0;  ///< `accept`: bytes per file_data packet
  bool ok = true;
  uint64_t size = 0;
  std::wstring name;
  std::wstring path;    ///< host path (done) or local path (downloaded)
  std::wstring message;
  std::wstring listing_path;
  std::wstring listing_root;
  std::vector<file_listing_entry> entries;
  /// `action == "chunk"`: one file_data payload, kept in stream order with the
  /// JSON messages so `eof` can never overtake the final bytes.
  uint64_t offset = 0;
  std::vector<uint8_t> chunk_data;
};

/// Row for the shell's transfer list.
struct file_transfer_state {
  uint32_t id = 0;
  std::wstring name;
  uint64_t size = 0;
  uint64_t sent = 0;
  bool download = false;
  int state = 0;  ///< 0 queued, 1 active, 2 done, 3 failed
};

struct session_endpoint {
  std::string host;      ///< remote host (for /ws/direct) or empty for a target
  int port = 0;
  std::string target;    ///< device id for /ws/connect, empty for a typed address
  std::string token;     ///< access code (hello token) or client token
  /// Smoke/testing: talk to the host core directly (/ws/session) instead of
  /// going through a local core's proxy. `token` is then the access code.
  bool direct_session = false;
  /// Per-launch client token of the local core (proxy mode). Appended as
  /// `&client=` to /ws/direct and /ws/connect.
  std::string client_token;
};

class session_client {
 public:
  /// Opens the session through the local core at `core_port`.
  bool start(const std::string& core_host, int core_port,
             const session_endpoint& endpoint, ui::session_stats* stats,
             std::string* error);
  /// Sends a ping when the interval elapsed; call from the UI timer.
  void tick(uint64_t now_ms);
  void stop();
  bool live() const { return ws_.is_open(); }
  /// Device name reported by the host in `welcome` (empty until then).
  const std::wstring& device_name() const { return device_name_; }
  /// `welcome.security`: e2e, relay or none (empty until welcome).
  const std::wstring& security() const { return security_; }
  /// True once the host sent `welcome` (the code was accepted or access was
  /// approved).
  bool authenticated() const { return authenticated_; }
  /// True after `request-access` until the host answers.
  bool access_pending() const { return access_pending_; }
  /// Last host `error` (code: message), empty when none.
  const std::wstring& last_error() const { return last_error_; }
  /// Host-global video path from `welcome` / `video-mode`:
  /// stream, mixed or tiles (empty until the host says).
  const std::wstring& video_mode() const { return video_mode_; }
  /// Host avatar from `welcome`: "preset:<colour>" or a data URL (may be
  /// empty). The shell caches it per recent device.
  const std::wstring& avatar() const { return avatar_; }
  /// Codec of the last frame the host actually sent (TILES / AV1 / VP9 / VP8 /
  /// H.264), empty before the first frame. The watermark shows this next to
  /// the configured preference.
  std::wstring live_codec_name() const;
  /// Sets the host's video path for every viewer (host-global setting).
  bool send_video_mode(const std::string& mode);
  /// Last socket close: code and reason text. The reason is what the host or
  /// the network said, and is written to the session log on reconnect.
  const std::wstring& close_detail() const { return close_detail_; }
  /// Machine-readable part of the last host `error` (invalid_code,
  /// access_denied, locked, ...), empty when none.
  const std::wstring& last_error_code() const { return last_error_code_; }
  /// Host cursor (`cursor-position`), in remote-screen pixels.
  int cursor_x() const { return cursor_x_.load(); }
  int cursor_y() const { return cursor_y_.load(); }
  bool cursor_visible() const { return cursor_visible_.load(); }
  int screen_width() const { return screen_width_.load(); }
  int screen_height() const { return screen_height_.load(); }
  /// Last `transport` announcement: path (direct/lan/relay) and protocol.
  const std::wstring& path_label() const { return path_label_; }
  const std::wstring& protocol_label() const { return protocol_label_; }
  /// Copies the newest decoded tile frame (BGRA) when `since` is older than
  /// the stored sequence. Returns false when nothing new arrived.
  bool copy_frame(std::vector<uint8_t>& out, int* width, int* height,
                  uint64_t* sequence, uint64_t since);
  /// Drains chat messages and clipboard text received from the host.
  std::vector<std::string> take_chat();
  std::vector<std::string> take_clipboard();

  // ---- File transfer (web `file` actions + file_data packets) ----
  /// Folder downloads are written to (`%USERPROFILE%\Downloads\moLink` by
  /// default). Missing directories are created on the first download.
  void set_download_dir(const std::wstring& dir);
  /// Queue a local file to send. Returns false when no session is live.
  bool file_offer(const std::wstring& name, const std::wstring& local_path,
                  const std::wstring& destination = std::wstring());
  /// Browse the host's home folder (empty path = home itself).
  void file_browse(const std::wstring& path);
  /// Ask the host to send a file inside its home folder.
  void file_request(const std::wstring& path);
  /// Cancel an upload/download by the host-assigned id (0 cancels the next
  /// queued offer).
  void file_cancel(uint32_t id);
  /// Pump the file protocol: send queued offers/chunks, write received chunks
  /// and answer pull requests. Call from the UI timer.
  void pump_files();
  /// File protocol messages (including synthesized `downloaded`).
  std::vector<file_event> take_file_events();
  /// Current uploads and downloads, newest last.
  std::vector<file_transfer_state> file_transfers();
  /// Forwards a quality change (mode/preset plus the slider values).
  void send_quality(const std::string& mode, int fps, int qp, int bitrate_kbps,
                    const std::string& rate_mode, const std::string& format);
  /// Raw JSON to the session (stream-codec, display-mode, audio, input...).
  bool send_json(const std::string& text);
  /// Tile requests after hello. Disable when connecting through a local core
  /// that decodes streams and publishes the native surface.
  void set_request_tiles(bool on) { request_tiles_ = on; }
  /// The viewer core decodes AV1/VP9/VP8 into the native surface. Advertise
  /// those codecs and ask the host for the AV1 stream instead of tiles.
  void set_stream_decoding(bool on) { stream_decoding_ = on; }
  /// The shell painted one frame (tile or surface); feeds the once-a-second
  /// viewer sample the host's QoS uses.
  void note_displayed_frame();
  /// The local core published a new frame on the shared surface. The shell
  /// never decodes stream frames itself, so this is its only "picture exists"
  /// signal; without it the first-frame bootstrap kept asking forever.
  void note_surface_frame();

 private:
  struct upload_state {
    uint32_t key = 0;       ///< local row key
    uint32_t host_id = 0;   ///< id from `accept`
    std::wstring name;
    std::wstring path;
    std::wstring dest;        ///< host destination folder ("" = ~/moLink)
    std::wstring message;
    std::wstring saved_path;  ///< host path from `done`
    uint64_t size = 0;
    uint64_t sent = 0;
    uint32_t chunk = 256 * 1024;
    int state = 0;          ///< 0 queued, 1 active, 2 done, 3 failed
    bool offer_sent = false;
    bool complete_sent = false;
    /// Set after the final chunk; `complete` goes out on the next pump so the
    /// core proxy cannot deliver it ahead of the last binary frame.
    bool complete_pending = false;
    std::ifstream in;
  };
  struct download_state {
    uint32_t id = 0;
    std::wstring name;
    std::wstring path;      ///< local path being written
    uint64_t size = 0;
    uint64_t received = 0;
    int state = 0;
    bool eof_seen = false;
    uint64_t eof_at = 0;
    std::ofstream out;
  };
  void handle_text(const std::string& text);
  void handle_binary(const uint8_t* data, size_t size);
  /// Records the capture timestamp a tile applied to every 128 px cell it
  /// covers, so the host's tile clock can name the cells this client missed.
  void note_applied_cells(int x, int y, int w, int h, uint64_t ts_us);
  /// Host `tile_clock` (0x15): asks for the cells whose capture timestamp is
  /// newer than the pixels on screen. The web does the same, which is what
  /// keeps a static desktop at zero traffic with no full-screen refreshes.
  void handle_tile_clock(const uint8_t* payload, uint32_t len);
  void handle_file_json(const std::string& text);
  /// Completes a download once `eof` and every byte have both been seen.
  void finish_download(uint32_t id);
  bool send_file_chunk(uint32_t id, uint64_t offset, const uint8_t* data,
                       size_t len);
  void send_file_action(const std::string& action, uint32_t id,
                        const std::string& extra);
  static bool find_number(const std::string& json, const char* key, int* out);
  static bool find_u64(const std::string& json, const char* key,
                       uint64_t* out);
  static bool find_string(const std::string& json, const char* key,
                          std::string* out);

  ws_client ws_;
  ui::session_stats* stats_ = nullptr;
  std::wstring device_name_;
  std::wstring security_;
  std::mutex frame_mutex_;
  std::vector<uint8_t> frame_pixels_;
  int frame_width_ = 0;
  int frame_height_ = 0;
  uint64_t frame_sequence_ = 0;
  bool stream_undecoded_ = false;
  std::mutex inbox_mutex_;
  std::vector<std::string> chat_inbox_;
  std::vector<std::string> clipboard_inbox_;
  // File protocol: the reader thread only queues messages/chunks; every
  // state change and byte of file I/O happens in pump_files() (UI thread).
  std::mutex file_mutex_;
  /// Reader -> pump, strictly ordered (JSON events and file bytes together).
  std::vector<file_event> file_events_;
  std::vector<file_event> shell_file_events_;  ///< pump -> shell
  std::map<uint32_t, upload_state> uploads_;
  std::map<uint32_t, download_state> downloads_;
  uint32_t next_upload_key_ = 1;
  uint32_t active_offer_key_ = 0;
  std::wstring download_dir_;
  uint64_t last_ping_ = 0;
  std::atomic<uint64_t> ping_sent_at_{0};
  /// Viewer health sample, once a second (web parity: the host's QoS uses it).
  std::atomic<uint64_t> stat_bytes_{0};
  uint64_t stat_frames_ = 0;
  uint64_t stat_window_start_ = 0;
  int last_rtt_ms_ = 0;
  bool request_tiles_ = true;
  bool tiles_requested_ = false;
  bool stream_decoding_ = false;
  bool authenticated_ = false;
  bool access_pending_ = false;
  /// The first hello/request-access can be lost while the relay/UDP path is
  /// still coming up; `hello_required` makes the host ask again.
  std::string hello_text_;
  bool access_request_ = false;
  int hello_resends_ = 0;
  uint64_t hello_sent_at_ = 0;
  /// One more frame request a second after welcome, in case the first one
  /// raced the stream/tile codec switch.
  uint64_t authenticated_at_ = 0;
  bool extra_frame_requested_ = false;
  /// Until the first picture arrives, ask for a frame/keyframe regularly: a
  /// host in tile mode only pushes changed cells, so a static desktop would
  /// otherwise never bootstrap a fresh viewer.
  std::atomic<bool> has_image_{false};
  uint64_t last_bootstrap_at_ = 0;
  int bootstrap_requests_ = 0;
  int screen_w_ = 0;
  int screen_h_ = 0;
  /// Tile mode: capture timestamp (us) applied to each 128 px cell, keyed by
  /// (row << 32) | col. Reader-thread only.
  std::unordered_map<uint64_t, uint64_t> applied_cells_;
  /// When the last tile-clock repair request went out (keep to one a second).
  uint64_t last_clock_request_ = 0;
  /// Last tile clock received from the host (reader thread -> UI tick).
  std::atomic<uint64_t> last_clock_seen_{0};
  /// Last full-screen refresh used as a fallback when clocks stop arriving.
  uint64_t last_refresh_at_ = 0;
  std::atomic<int> cursor_x_{0};
  std::atomic<int> cursor_y_{0};
  std::atomic<bool> cursor_visible_{false};
  std::atomic<int> screen_width_{0};
  std::atomic<int> screen_height_{0};
  std::wstring last_error_;
  std::wstring last_error_code_;
  std::wstring video_mode_;
  std::wstring avatar_;
  std::atomic<uint8_t> live_codec_{0};
  std::wstring close_detail_;
  std::wstring path_label_;
  std::wstring protocol_label_;
};

}  // namespace molink::net
