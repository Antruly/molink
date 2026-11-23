// Session wiring for the native shell: connects the WS client to the local
// core, sends hello, and feeds the toolbar's session_stats from the traffic.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "clipboard_files.h"
#include "preset_message.h"
#include "ui_kit.h"
#include "ws_client.h"

namespace molink::net {

/// One `file action=listing` entry (name is a single path segment).
struct file_listing_entry {
  std::wstring name;
  bool dir = false;
  uint64_t size = 0;
  uint64_t mtime = 0;
  /// Roots view: the absolute volume path paired with the row, and its kind
  /// (`fixed`, `removable`, `network`, `home`, ...).
  std::wstring path;
  std::wstring kind;
  /// Symlink/junction: list it, but a recursive walk never descends it.
  bool link = false;
};

/// One volume/root from a listing's `roots` array.
struct file_listing_root {
  std::wstring name;
  std::wstring path;
  std::wstring kind;
};

/// File-protocol message surfaced to the shell.
struct file_event {
  std::wstring action;  ///< listing/sending/eof/done/reject/cancel/error
                        ///< (the shell also synthesizes `downloaded`)
  uint32_t id = 0;
  uint32_t task = 0;   ///< UI batch the event belongs to (synthesized rows)
  uint32_t chunk = 0;  ///< `accept`: bytes per file_data packet
  bool ok = true;
  uint64_t size = 0;
  std::wstring name;
  std::wstring path;    ///< host path (done) or local path (downloaded)
  std::wstring message;
  std::wstring listing_path;
  std::wstring listing_root;
  std::wstring listing_parent;  ///< "" at a volume/share root
  bool truncated = false;
  std::vector<file_listing_root> roots;
  std::vector<file_listing_entry> entries;
  /// `action == "walk-done"`: the synthetic end marker of a recursive
  /// download. It never travels on the wire; `session_client` emits it after
  /// the last folder of one `file_download_tree` has been listed.
  int walk_files = 0;
  int walk_dirs = 0;
  int walk_skipped = 0;
  /// `action == "chunk"`: one file_data payload, kept in stream order with the
  /// JSON messages so `eof` can never overtake the final bytes.
  uint64_t offset = 0;
  std::vector<uint8_t> chunk_data;
  /// `action == "fetch"`: host's virtual-file request id, offered index and
  /// range length. `offset` above is the requested offset.
  uint32_t index = 0;
  uint64_t length = 0;
  /// `clipboard-pull-chunk` (0x18): 0 ok, 1 read error, 2 clipboard changed.
  uint32_t status = 0;
  /// `clipboard-pull-offer`: the host's `clipboard-sync.files` list, the
  /// number it omitted, and whether an inner RDP client owns its clipboard.
  std::vector<molink::clipboard_files::entry> clip_files;
  std::size_t clip_omitted = 0;
  bool clip_nested_rdp = false;
  /// `clipboard-done`: how many files were placed on the local clipboard.
  int count = 0;
  /// `clipboard-offer` reply: the host will fetch ranges instead of asking
  /// for an eager upload.
  bool stream = false;
};

/// Last diagnostics answer from the host (`logs tail` / `logs info`).
struct host_log_state {
  bool has_tail = false;
  std::string tail;       ///< UTF-8 text, already truncated by the host
  uint64_t tail_size = 0;
  int tail_lines = 0;
  uint64_t tail_at = 0;   ///< GetTickCount64 when the tail arrived
  bool has_info = false;
  bool enabled = false;
  std::string directory;
  int retention_days = 7;
  uint64_t current_bytes = 0;
  std::string file;
  uint64_t info_at = 0;   ///< GetTickCount64 when the info arrived
  /// `welcome`: the host's version, shown next to its port and log path.
  std::string version;
  /// The port this client tried to reach. Zero for an ID-only target.
  int port = 0;
};

/// Row for the shell's transfer list.
struct file_transfer_state {
  uint32_t id = 0;
  uint32_t task = 0;  ///< UI-selected batch id (0 = standalone row)
  std::wstring name;
  uint64_t size = 0;
  uint64_t sent = 0;
  bool download = false;
  bool mkdir = false;  ///< an empty-folder create, not a file row
  int state = 0;  ///< 0 queued, 1 active, 2 done, 3 failed/cancelled
  bool cancelled = false;
  std::wstring message;  ///< host reject/error text, or the local reason
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
  /// stream or tiles (empty until the host says).
  const std::wstring& video_mode() const { return video_mode_; }
  /// Reconnect credential from the host's `welcome` (`session_token`). The
  /// shell keeps it per target+code and hands it to the *next* session object,
  /// so a reconnect does not spend another use of a single-use code. Empty
  /// until the first welcome.
  const std::string& resume_token() const { return resume_token_; }
  /// Sets the resume credential before `start()`. Each connection still owns
  /// its own session_client and socket; only the token is carried over.
  void set_resume_token(const std::string& token) { resume_token_ = token; }
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
  /// Bumped on every host error/close-policy answer so the UI can tell a new
  /// occurrence from the same text still sitting in `last_error()`.
  unsigned error_seq() const { return error_seq_.load(); }
  /// Feature failures (input/clipboard/file/settings) are kept on a separate
  /// channel: a `bad_input` must never overwrite the banner's credential error
  /// (and turn its code field into a plain reconnect button).
  const std::wstring& last_feature_error() const { return last_feature_error_; }
  const std::wstring& last_feature_error_code() const {
    return last_feature_error_code_;
  }
  unsigned feature_error_seq() const { return feature_error_seq_.load(); }
  /// Host diagnostics (E2). Tail and info are privileged and require an
  /// authenticated session; the state is copied under a mutex because the
  /// reader thread writes it and the UI polls it.
  bool request_log_tail(int lines = 500);
  bool request_log_info();
  bool set_host_logging(bool enabled);
  host_log_state host_logs() const;
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
  /// Queue a local file to send. Returns false when no session is live, the
  /// same name/destination is already queued, or the file exceeds 8 GiB.
  /// `name` may be a slash-separated path (`photos/a.jpg`); the host creates
  /// the relative parents under `destination`.
  bool file_offer(const std::wstring& name, const std::wstring& local_path,
                  const std::wstring& destination = std::wstring(),
                  uint32_t task = 0);
  /// Queue an explicit empty-folder create under `destination`. Web
  /// `file action=mkdir`; the host answers with a `mkdir` event.
  bool file_mkdir(const std::wstring& relative,
                  const std::wstring& destination,
                  uint32_t task = 0);
  /// Browse a host folder. An empty path is the volume list (the panel's
  /// "roots view"); a non-empty path is resolved by the host against home.
  void file_browse(const std::wstring& path);
  /// Ask the host to send a file. `task` groups several requests made by one
  /// "传回来" click so the panel can show one task row.
  void file_request(const std::wstring& path, uint32_t task = 0);
  /// Ask for a file that belongs to a recursive download. `relative` (slash
  /// separated, relative to the download root) decides where it is written:
  /// the remote tree is recreated under the download directory.
  bool file_request_relative(const std::wstring& path,
                             const std::wstring& relative,
                             uint32_t task = 0);
  /// Download a remote folder recursively. Every file is requested with its
  /// path below `relative`, and empty folders are created locally. One
  /// `walk-done` event closes the task. Returns false with no live session.
  bool file_download_tree(const std::wstring& path,
                          const std::wstring& relative,
                          uint32_t task = 0);
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
  /// The local clipboard gained a file list (CF_HDROP). Filters echoes,
  /// caps the selection at the host's 8 files / 2 GiB delayed-render limit
  /// and sends `{"type":"file","action":"clipboard-offer"}`; the bytes travel
  /// only when the host answers with a `fetch` after a paste. Returns false
  /// when nothing was offered (not authenticated, empty, duplicate, echo).
  bool clipboard_files_changed(const std::vector<std::string>& paths);
  /// The local clipboard no longer holds files (text/empty). Allows the same
  /// file selection to be offered again after the host's lazy clipboard was
  /// replaced; the existing offer stays answerable until a new one replaces
  /// it.
  void clipboard_files_cleared();
  /// `welcome.clipboard_stream` / `clipboard_pull`: the host confirms it can
  /// render an offer lazily and serve an id-based pull. `nested_rdp` is a
  /// badge only and no longer disables either path.
  bool host_clipboard_stream() const { return host_clipboard_stream_; }
  bool host_clipboard_pull() const { return host_clipboard_pull_; }
  bool host_nested_rdp() const { return host_nested_rdp_; }
  /// True while an inbound clipboard-file batch is still moving bytes, so the
  /// drawer repaints its per-file progress even when no JSON event arrived.
  bool clipboard_pull_active() const {
    return clipboard_pull_ != nullptr && !clipboard_pull_->complete &&
           !clipboard_pull_->failed && !clipboard_pull_->cancelled;
  }
  /// Sends a viewer-owned `session-preset` message (the JSON is built by
  /// `molink::preset`) and asks for a keyframe afterwards, like the Web's
  /// `requestKeyframe()` after a tier change. Returns false when the socket
  /// is closed.
  bool send_session_preset(const std::string& json);
  /// The preset sent on `welcome`, before the first frame. The shell resolves
  /// the selected tier (or custom values) with `molink::preset` and installs
  /// it before connecting; the default is the Web's smooth tier.
  void set_startup_preset(const std::string& json) {
    startup_preset_json_ = json;
  }
  /// Last `session-preset` reply (or the retired `quality` acknowledgement
  /// as a fallback). `applied` stays false until the host confirms; the
  /// panel shows this without moving the user's selector.
  preset::applied_reply applied_preset() const;
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
    uint32_t task = 0;      ///< UI batch that started this row
    std::wstring name;
    std::wstring path;
    std::wstring dest;        ///< host destination folder ("" = ~/moLink)
    std::wstring message;
    std::wstring saved_path;  ///< host path from `done`
    uint64_t size = 0;
    uint64_t sent = 0;
    uint32_t chunk = 256 * 1024;
    int state = 0;          ///< 0 queued, 1 active, 2 done, 3 failed
    bool cancelled = false;
    bool offer_sent = false;
    bool complete_sent = false;
    /// Empty-folder create: sent as `mkdir`, has no local input or bytes.
    bool mkdir = false;
    /// When the offer/mkdir went out, so a lost answer cannot wedge the
    /// single active-upload slot forever.
    uint64_t sent_at = 0;
    /// Set after the final chunk; `complete` goes out on the next pump so the
    /// core proxy cannot deliver it ahead of the last binary frame.
    bool complete_pending = false;
    std::ifstream in;
  };
  struct download_state {
    uint32_t id = 0;
    uint32_t task = 0;      ///< UI batch that requested this row
    std::wstring name;
    std::wstring path;      ///< local path being written
    std::wstring rel;       ///< remote tree path ("" = a flat download)
    std::wstring remote_path;  ///< for duplicate-request detection
    std::wstring message;
    uint64_t size = 0;
    uint64_t received = 0;
    int state = 0;
    bool cancelled = false;
    bool eof_seen = false;
    uint64_t eof_at = 0;
    uint64_t last_progress_at = 0;
    std::ofstream out;
  };
  /// One file the host announced through `clipboard-sync.files`, being pulled
  /// in 256 KiB ranges. The bytes land in a `.part` file and are renamed only
  /// after the whole batch is complete, so a half file can never reach the
  /// clipboard.
  struct clipboard_pull_file {
    uint32_t id = 0;
    /// One drawer task per file, so the receive list shows a line (name +
    /// progress bar) per clipboard file rather than one aggregate card.
    uint32_t task = 0;
    std::string name;
    uint64_t size = 0;
    uint64_t received = 0;
    std::wstring temp_path;
    std::wstring final_path;
    std::ofstream out;
    int state = 0;  ///< 0 queued, 1 active, 2 done, 3 failed/cancelled
    bool cancelled = false;
    molink::clipboard_files::range_retry retry;
    uint64_t requested_at = 0;
    std::wstring message;
  };
  struct clipboard_pull_batch {
    uint32_t sequence = 0;
    uint64_t total = 0;
    uint64_t received = 0;
    std::size_t omitted = 0;
    bool nested_rdp = false;
    bool failed = false;
    bool cancelled = false;
    bool complete = false;
    int active = -1;
    std::wstring directory;
    std::vector<clipboard_pull_file> files;
  };
  /// A local file described to the host by `clipboard-offer`. `path` is read
  /// only when the host answers a paste with `fetch`.
  struct clipboard_offered_file {
    std::string name;
    std::wstring path;
    uint64_t size = 0;
  };
  /// One recursive download. Folders are listed one at a time; each listing
  /// queues its child folders and sends a `request` for every file.
  struct download_walk {
    uint32_t task = 0;
    struct folder {
      std::wstring path;  ///< remote path sent to `browse`
      std::wstring rel;   ///< local path below the download root
    };
    std::vector<folder> pending;
    std::size_t next = 0;
    std::size_t entries = 0;
    int files = 0;
    int dirs = 0;
    int skipped = 0;
    bool truncated = false;
  };
  /// A browse whose answer is either shown (user navigation) or consumed by
  /// a recursive walk. The host answers in order, so a FIFO matches them.
  struct pending_browse {
    std::wstring path;
    bool walk = false;
    uint32_t walk_id = 0;
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
  /// `clipboard-sync.files`: plan the list (8 / 1 TiB / 4 TiB), create the
  /// staging folder and open the `.part` files. Called on the UI thread from
  /// `pump_files()`.
  void start_clipboard_pull(const file_event& offer);
  /// Sends the next `clipboard-pull` range, or the next queued file; also
  /// enforces the per-range timeout. Called from `pump_files()`.
  void pump_clipboard_pull(uint64_t now_ms);
  /// One `0x18` reply: validate the range, write it, then ask for the rest.
  void handle_clipboard_chunk(const file_event& event);
  /// Host paste on a lazy offer: answer `fetch` with a `0x17` range packet.
  void handle_clipboard_fetch(const file_event& event);
  void request_clipboard_range(clipboard_pull_file& file, uint64_t now_ms);
  void advance_clipboard_pull(uint64_t now_ms);
  /// Every byte arrived: rename `.part` files, put the final paths on the
  /// local clipboard and tell the shell.
  void finish_clipboard_pull();
  /// Abandon the active batch and delete every temporary/final file. The rows
  /// stay visible with their failure reason.
  void fail_clipboard_pull(const std::wstring& message);
  /// Drop the batch and delete its files (a newer selection supersedes it).
  void discard_clipboard_pull();
  /// User cancellation from the file drawer's synthetic row id.
  void cancel_clipboard_pull();
  std::wstring clipboard_base_directory() const;
  std::wstring clipboard_batch_directory(uint32_t sequence) const;
  bool set_clipboard_files(const std::vector<std::wstring>& paths);
  bool send_clipboard_range(uint32_t id, uint32_t code, uint64_t offset,
                            const uint8_t* data, size_t len);
  /// Queues a shell event that the UI turns into a toast. `action` is
  /// `clipboard-done`, `clipboard-warn` or `clipboard-error`.
  void queue_clipboard_toast(const wchar_t* action,
                             const std::wstring& message, int count = 0);
  static constexpr uint32_t kClipboardTaskFlag = 0x40000000u;
  static constexpr uint32_t kClipboardRowFlag = 0x20000000u;
  /// Completes a download once `eof` and every byte have both been seen.
  void finish_download(uint32_t id);
  /// Defaults `download_dir_` and creates it; returns false when unavailable.
  bool ensure_download_root();
  /// Creates the local folder chain for a slash-separated relative path.
  bool ensure_local_directory(const std::wstring& relative);
  /// The local file path for a download: safe segments below `download_dir_`.
  /// Returns "" when the path cannot be represented safely.
  std::wstring local_download_path(const std::wstring& relative,
                                   std::wstring* display_name);
  /// Sends `request` for a file and queues the matching `sending` answer.
  bool queue_download_request(const std::wstring& path,
                              const std::wstring& relative, uint32_t task);
  /// Starts queued `request`s up to the host's eight download slots.
  void pump_download_requests();
  /// Sends the next `browse` of a recursive download, or emits `walk-done`.
  void advance_download_walk(uint32_t walk_id);
  /// Releases the queued-name slot when an upload finishes, so the same file
  /// can be sent again after a failure.
  void release_upload_slot(const upload_state& upload);
  /// Consumes one listing answer for a recursive download.
  void handle_walk_listing(const file_event& event, uint32_t walk_id);
  pending_browse take_pending_browse();
  bool send_file_chunk(uint32_t id, uint64_t offset, const uint8_t* data,
                       size_t len);
  bool send_file_action(const std::string& action, uint32_t id,
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
  /// `file_request` paths waiting for the host's `sending` answer. The host
  /// handles requests in order, so the oldest entry owns the next row; a name
  /// match keeps a rejected request from shifting the queue.
  struct pending_download {
    std::wstring path;
    std::wstring rel;  ///< remote-tree path ("" = flat download)
    uint32_t task = 0;
  };
  std::vector<pending_download> pending_downloads_;
  /// Queued/active `dest + name` pairs, so accepting a 20000-file folder does
  /// not rescan every row for each offer.
  std::set<std::wstring> upload_slots_;
  /// Requests not sent yet, because the host keeps only eight downloads open.
  std::deque<pending_download> request_queue_;
  int active_requests_ = 0;  ///< sent and not yet finished (any state)
  /// Browse answers are consumed in the order the requests were sent.
  std::deque<pending_browse> pending_browses_;
  std::map<uint32_t, download_walk> download_walks_;
  uint32_t next_walk_id_ = 1;
  /// Inbound clipboard-file pull (host -> this machine's clipboard).
  std::unique_ptr<clipboard_pull_batch> clipboard_pull_;
  std::string clipboard_session_;
  uint32_t clipboard_batch_seq_ = 0;
  uint32_t next_clipboard_task_ = 1;
  /// Outbound delayed offer (this machine's clipboard -> host). The bytes are
  /// read from `path` only when the host sends `fetch`.
  std::vector<clipboard_offered_file> clipboard_offered_;
  std::string clipboard_offer_key_;
  /// Two-minute suppression for paths/descriptors this machine just handled.
  molink::clipboard_files::file_echo clipboard_echo_;
  bool host_clipboard_stream_ = false;
  bool host_clipboard_pull_ = false;
  bool host_nested_rdp_ = false;
  uint32_t next_upload_key_ = 1;
  /// Lowest upload key not offered yet; keys complete in order, so the pump
  /// never has to rescan a 20000-file queue.
  uint32_t next_offer_key_ = 1;
  /// Failed requests carry host id 0; they still need a unique row key.
  uint32_t next_download_key_ = 1;
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
  /// Incremented after `last_error_`/`last_error_code_` change.
  std::atomic<unsigned> error_seq_{0};
  /// The feature channel (toast only).
  std::wstring last_feature_error_;
  std::wstring last_feature_error_code_;
  std::atomic<unsigned> feature_error_seq_{0};
  /// Host diagnostics state; guarded by `log_mutex_` because handle_text()
  /// runs on the reader thread while the UI polls it.
  mutable std::mutex log_mutex_;
  host_log_state host_logs_;
  std::wstring video_mode_;
  std::string resume_token_;
  std::wstring avatar_;
  std::atomic<uint8_t> live_codec_{0};
  std::wstring close_detail_;
  std::wstring path_label_;
  std::wstring protocol_label_;
  /// The viewer's selected picture preset, sent once on `welcome` (Web sends
  /// it before the first frame) and again on every change. Defaults to the
  /// Web's smooth tier; the shell installs the real selection before start().
  std::string startup_preset_json_ =
      preset::preset_message_json(preset::default_preset(), 0);
  bool startup_preset_sent_ = false;
  /// Reader thread writes; the UI thread reads a copy.
  mutable std::mutex applied_mutex_;
  preset::applied_reply applied_;
};

}  // namespace molink::net
