#include "terminal/terminal_service.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>

#include "diag_log.h"

namespace molink {
namespace {

constexpr std::size_t kMaxReadPerPoll = 256u * 1024u;

std::string dimension_text(int value) { return std::to_string(value); }

}  // namespace

terminal_ring_buffer::terminal_ring_buffer(std::size_t capacity)
    : buffer_(capacity == 0 ? 1 : capacity) {}

void terminal_ring_buffer::append(const uint8_t* data, std::size_t len) {
  if (len == 0) return;
  const std::size_t capacity = buffer_.size();
  seq_ += len;
  if (len >= capacity) {
    // The append alone evicts everything older; only the tail survives.
    std::memcpy(buffer_.data(), data + (len - capacity), capacity);
    head_ = 0;
    size_ = capacity;
    return;
  }
  const std::size_t free_bytes = capacity - size_;
  if (len > free_bytes) {
    const std::size_t drop = len - free_bytes;
    head_ = (head_ + drop) % capacity;
    size_ -= drop;
  }
  const std::size_t tail = (head_ + size_) % capacity;
  const std::size_t first = (std::min)(len, capacity - tail);
  std::memcpy(buffer_.data() + tail, data, first);
  if (len > first) {
    std::memcpy(buffer_.data(), data + first, len - first);
  }
  size_ += len;
}

void terminal_ring_buffer::clear() {
  head_ = 0;
  size_ = 0;
  seq_ = 0;
}

std::vector<uint8_t> terminal_ring_buffer::replay(uint64_t from_seq,
                                                  uint64_t& start_seq,
                                                  bool& truncated) const {
  start_seq = seq_;
  truncated = false;
  const std::size_t capacity = buffer_.size();
  if (from_seq >= seq_ || size_ == 0) return {};
  const uint64_t oldest = oldest_seq();
  uint64_t start = from_seq;
  if (start < oldest) {
    truncated = true;
    start = oldest;
  }
  const std::size_t offset = static_cast<std::size_t>(start - oldest);
  if (offset >= size_) return {};
  const std::size_t count = size_ - offset;
  const std::size_t index = (head_ + offset) % capacity;
  const std::size_t first = (std::min)(count, capacity - index);
  std::vector<uint8_t> out(count);
  std::memcpy(out.data(), buffer_.data() + index, first);
  if (count > first) {
    std::memcpy(out.data() + first, buffer_.data(), count - first);
  }
  start_seq = start;
  return out;
}

class terminal_session {
 public:
  terminal_session(uint64_t id, pty_handle handle, std::string shell, int cols,
                   int rows, std::size_t ring_bytes)
      : id_(id),
        handle_(handle),
        shell_(std::move(shell)),
        cols_(cols),
        rows_(rows),
        output_(ring_bytes) {}

  ~terminal_session() { pty_kill(handle_); }

  terminal_session(const terminal_session&) = delete;
  terminal_session& operator=(const terminal_session&) = delete;

  uint64_t id() const { return id_; }
  pty_handle& handle() { return handle_; }
  terminal_ring_buffer& output() { return output_; }
  const std::string& shell() const { return shell_; }
  int cols() const { return cols_; }
  int rows() const { return rows_; }
  bool exited() const { return handle_.reaped; }
  int exit_code() const { return handle_.exit_code; }

 private:
  uint64_t id_;
  pty_handle handle_;
  std::string shell_;
  int cols_;
  int rows_;
  terminal_ring_buffer output_;
};

struct terminal_service::entry {
  std::unique_ptr<terminal_session> session;
  /// Viewer that opened the terminal. A viewer disconnect reclaims exactly
  /// the sessions it created; terminals it merely attached to keep running.
  uint64_t owner = 0;
  std::set<uint64_t> viewers;
  std::chrono::steady_clock::time_point last_active;
};

terminal_service::terminal_service(std::size_t ring_bytes,
                                   std::chrono::milliseconds idle_timeout)
    : ring_bytes_(ring_bytes == 0 ? kDefaultRingBytes : ring_bytes),
      idle_timeout_(idle_timeout) {}

terminal_service::~terminal_service() {
  for (auto& item : sessions_) {
    if (item.second->session && !item.second->session->exited()) {
      pty_kill(item.second->session->handle());
    }
  }
  sessions_.clear();
}

terminal_service::entry* terminal_service::find(uint64_t id) {
  const auto it = sessions_.find(id);
  return it == sessions_.end() ? nullptr : it->second.get();
}

const terminal_service::entry* terminal_service::find(uint64_t id) const {
  const auto it = sessions_.find(id);
  return it == sessions_.end() ? nullptr : it->second.get();
}

uint64_t terminal_service::register_viewer() { return next_viewer_++; }

void terminal_service::release_viewer(uint64_t viewer) {
  // A viewer that goes away (WS closed, page reloaded) must not leave the
  // processes it opened behind. Sessions it merely attached to belong to
  // another viewer and keep running; a host-side disconnect is covered by the
  // idle timeout instead.
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    entry& value = *it->second;
    value.viewers.erase(viewer);
    if (value.owner != viewer) {
      ++it;
      continue;
    }
    const uint64_t id = it->first;
    if (value.session && !value.session->exited()) {
      pty_kill(value.session->handle());
    }
    std::vector<diag_field> fields;
    fields.push_back({"id", std::to_string(id)});
    fields.push_back({"reason", "disconnect"});
    diag().log(diag_level::info, "term.reclaim",
               "viewer disconnected; terminal reclaimed", fields);
    it = sessions_.erase(it);
  }
}

std::size_t terminal_service::live_count(uint64_t viewer) const {
  std::size_t count = 0;
  for (const auto& item : sessions_) {
    const entry& value = *item.second;
    if (value.viewers.count(viewer) != 0 && value.session &&
        !value.session->exited()) {
      ++count;
    }
  }
  return count;
}

uint64_t terminal_service::open(uint64_t viewer, const std::string& shell,
                                int cols, int rows, std::string& err) {
  err.clear();
  if (live_count(viewer) >= kMaxViewerTerminals) {
    err = "at most 4 terminals can be open per viewer";
    diag().log(diag_level::warn, "term.rejected",
               "per-viewer terminal limit reached",
               {{"reason", "limit"},
                {"viewer", std::to_string(viewer)},
                {"limit", std::to_string(kMaxViewerTerminals)}});
    return 0;
  }

  // Pass the request through unchanged: an empty shell asks the platform for
  // its default. On Windows the helper then picks the *console user's*
  // %COMSPEC% (from the user environment block it builds); resolving it here
  // with the service's own environment would defeat that.
  const std::string command = shell;
  std::string spawn_error;
  pty_handle handle = pty_spawn(command, cols, rows, spawn_error);
  if (!pty_valid(handle)) {
    err = spawn_error.empty() ? "failed to spawn terminal" : spawn_error;
    diag().log(diag_level::warn, "term.rejected",
               "terminal spawn failed",
               {{"reason", "spawn"},
                {"viewer", std::to_string(viewer)},
                {"shell", command},
                {"error", err}});
    return 0;
  }

  // Only a display value for term.open; pty_spawn resolved the real default.
  const std::string shown_shell =
      command.empty() ? pty_default_shell() : command;
  const uint64_t id = next_id_++;
  auto entry = std::make_unique<terminal_service::entry>();
  entry->session = std::make_unique<terminal_session>(
      id, handle, shown_shell, cols, rows, ring_bytes_);
  entry->owner = viewer;
  entry->viewers.insert(viewer);
  entry->last_active = std::chrono::steady_clock::now();
  sessions_[id] = std::move(entry);

  diag().log(diag_level::info, "term.open", std::string(),
             {{"id", std::to_string(id)},
              {"shell", shown_shell},
              {"cols", dimension_text(cols)},
              {"rows", dimension_text(rows)}});
  return id;
}

bool terminal_service::attach(uint64_t viewer, uint64_t id, uint64_t from_seq,
                              std::vector<uint8_t>& replay,
                              uint64_t& replay_seq, bool& truncated,
                              std::string& err) {
  err.clear();
  entry* value = find(id);
  if (value == nullptr) {
    err = "terminal not found";
    return false;
  }
  if (value->viewers.count(viewer) == 0 &&
      live_count(viewer) >= kMaxViewerTerminals) {
    err = "at most 4 terminals can be open per viewer";
    diag().log(diag_level::warn, "term.rejected",
               "per-viewer terminal limit reached",
               {{"reason", "limit"},
                {"viewer", std::to_string(viewer)},
                {"id", std::to_string(id)}});
    return false;
  }
  value->viewers.insert(viewer);
  value->last_active = std::chrono::steady_clock::now();
  replay = value->session->output().replay(from_seq, replay_seq, truncated);
  return true;
}

void terminal_service::detach(uint64_t viewer, uint64_t id) {
  entry* value = find(id);
  if (value != nullptr) value->viewers.erase(viewer);
}

bool terminal_service::write(uint64_t id, const uint8_t* data,
                             std::size_t len) {
  entry* value = find(id);
  if (value == nullptr || value->session->exited() || len == 0) return false;
  value->last_active = std::chrono::steady_clock::now();
  return pty_write(value->session->handle(), data, len) ==
         static_cast<int>(len);
}

bool terminal_service::resize(uint64_t id, int cols, int rows) {
  entry* value = find(id);
  if (value == nullptr || value->session->exited()) return false;
  value->last_active = std::chrono::steady_clock::now();
  return pty_resize(value->session->handle(), cols, rows);
}

bool terminal_service::close(uint64_t id, const char* reason) {
  const auto it = sessions_.find(id);
  if (it == sessions_.end()) return false;
  entry& value = *it->second;
  const bool was_live = value.session && !value.session->exited();
  int code = value.session ? value.session->exit_code() : -1;
  if (was_live) pty_kill(value.session->handle());
  code = value.session ? value.session->exit_code() : code;
  if (was_live) {
    std::vector<diag_field> fields;
    fields.push_back({"id", std::to_string(id)});
    fields.push_back({"code", std::to_string(code)});
    if (reason != nullptr && reason[0] != '\0') {
      fields.push_back({"reason", reason});
    }
    diag().log(diag_level::info, "term.close", std::string(), fields);
  }
  sessions_.erase(it);
  return true;
}

void terminal_service::poll() {
  const auto now = std::chrono::steady_clock::now();
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    entry& value = *it->second;
    const uint64_t id = it->first;
    if (!value.session->exited()) {
      std::size_t read_total = 0;
      while (read_total < kMaxReadPerPoll) {
        uint8_t buffer[65536];
        const int n = pty_read(value.session->handle(), buffer,
                               sizeof(buffer));
        if (n > 0) {
          const uint64_t seq = value.session->output().seq();
          value.session->output().append(buffer, static_cast<std::size_t>(n));
          value.last_active = now;
          read_total += static_cast<std::size_t>(n);
          notify_output(id, seq, buffer, static_cast<std::size_t>(n));
          continue;
        }
        if (n < 0) {
#if defined(EINTR)
          if (errno == EINTR) continue;
#endif
          // EAGAIN, EOF and EIO all mean "nothing more right now"; waitpid
          // below decides whether the process is actually gone.
          break;
        }
        break;
      }
      int code = -1;
      if (pty_poll_exit(value.session->handle(), code)) {
        std::vector<diag_field> fields;
        fields.push_back({"id", std::to_string(id)});
        fields.push_back({"code", std::to_string(code)});
        diag().log(diag_level::info, "term.close", std::string(), fields);
        notify_exit(id, code);
      }
    }

    if (now - value.last_active >= idle_timeout_) {
      if (!value.session->exited()) {
        pty_kill(value.session->handle());
        diag().log(diag_level::info, "term.idle_close",
                   "terminal idle timeout",
                   {{"id", std::to_string(id)},
                    {"code", std::to_string(value.session->exit_code())},
                    {"reason", "idle"}});
      }
      it = sessions_.erase(it);
      continue;
    }
    ++it;
  }
}

uint64_t terminal_service::add_listener(output_fn on_output, exit_fn on_exit) {
  const uint64_t token = next_listener_++;
  listeners_[token] = {std::move(on_output), std::move(on_exit)};
  return token;
}

void terminal_service::remove_listener(uint64_t token) {
  listeners_.erase(token);
}

bool terminal_service::has_session(uint64_t id) const {
  return find(id) != nullptr;
}

bool terminal_service::exit_code(uint64_t id, int& code) const {
  const entry* value = find(id);
  if (value == nullptr || value->session == nullptr ||
      !value->session->exited()) {
    return false;
  }
  code = value->session->exit_code();
  return true;
}

std::size_t terminal_service::session_count() const { return sessions_.size(); }

void terminal_service::notify_output(uint64_t id, uint64_t seq,
                                     const uint8_t* data, std::size_t len) {
  for (const auto& item : listeners_) {
    if (item.second.first) item.second.first(id, seq, data, len);
  }
}

void terminal_service::notify_exit(uint64_t id, int code) {
  for (const auto& item : listeners_) {
    if (item.second.second) item.second.second(id, code);
  }
}

}  // namespace molink
