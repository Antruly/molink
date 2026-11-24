#include "clipboard_route.h"

#include <algorithm>

#include "util.h"

namespace molink {
namespace {

bool older_than(uint64_t now_ms, uint64_t at, uint64_t ttl_ms) {
  // A clock that stepped backwards keeps the entry rather than expiring
  // everything; going back in time is not proof the value is old.
  return now_ms >= at && now_ms - at >= ttl_ms;
}

/// Most recently active first: input beats connection time.
bool more_recent(const clipboard_route_session& left,
                 const clipboard_route_session& right) {
  if (left.last_input_ms != right.last_input_ms) {
    return left.last_input_ms > right.last_input_ms;
  }
  return left.connected_ms > right.connected_ms;
}

}  // namespace

const char* clipboard_owner_name(clipboard_owner owner) {
  switch (owner) {
    case clipboard_owner::self:
      return "self";
    case clipboard_owner::external:
      return "external";
    case clipboard_owner::unknown:
      break;
  }
  return "unknown";
}

const char* clipboard_route_reason_name(clipboard_route_reason reason) {
  switch (reason) {
    case clipboard_route_reason::foreground:
      return "foreground";
    case clipboard_route_reason::input:
      return "input";
    case clipboard_route_reason::sole:
      return "sole";
    case clipboard_route_reason::broadcast:
      return "broadcast";
    case clipboard_route_reason::no_target:
      return "no_target";
    case clipboard_route_reason::none:
      break;
  }
  return "none";
}

bool clipboard_foreground_active(uint64_t foreground_ms, uint64_t now_ms) {
  if (foreground_ms == 0) return false;
  // A clock that stepped backwards keeps the lease rather than expiring it.
  return now_ms < foreground_ms ||
         now_ms - foreground_ms < kClipboardForegroundTtlMs;
}

bool clipboard_foreground_active(const clipboard_route_session& session,
                                 uint64_t now_ms) {
  return clipboard_foreground_active(session.foreground_ms, now_ms);
}

bool clipboard_peer_may_see_paths(const std::string& peer_ip) {
  return is_loopback_peer(peer_ip);
}

bool clipboard_watcher_should_report(bool new_change,
                                     const std::string& content,
                                     const std::string& last_content) {
  if (content.empty()) return false;
  return new_change || content != last_content;
}

clipboard_route select_clipboard_targets(
    const std::vector<clipboard_route_session>& sessions, uint64_t now_ms,
    uint64_t input_window_ms, bool allow_broadcast) {
  clipboard_route route;
  std::vector<const clipboard_route_session*> live;
  live.reserve(sessions.size());
  for (const clipboard_route_session& session : sessions) {
    if (!session.authenticated || !session.connected) continue;
    live.push_back(&session);
  }
  if (live.empty()) return route;

  // 1. The viewer renewed the visible-tab lease. Two windows can both be
  // visible; the one that interacted most recently is the one the user is
  // working in. An expired lease is not a foreground signal.
  const clipboard_route_session* active = nullptr;
  for (const clipboard_route_session* session : live) {
    if (!clipboard_foreground_active(*session, now_ms)) continue;
    if (active == nullptr || more_recent(*session, *active)) active = session;
  }
  if (active != nullptr) {
    route.targets.push_back(active->id);
    route.active = active->id;
    route.reason = clipboard_route_reason::foreground;
    return route;
  }

  // 2. No foreground signal: use the most recent input, within the window.
  for (const clipboard_route_session* session : live) {
    if (session->last_input_ms == 0) continue;
    if (now_ms < session->last_input_ms ||
        now_ms - session->last_input_ms > input_window_ms) {
      continue;
    }
    if (active == nullptr || more_recent(*session, *active)) active = session;
  }
  if (active != nullptr) {
    route.targets.push_back(active->id);
    route.active = active->id;
    route.reason = clipboard_route_reason::input;
    return route;
  }

  // 3. One viewer is the active one by definition.
  if (live.size() == 1) {
    route.targets.push_back(live.front()->id);
    route.active = live.front()->id;
    route.reason = clipboard_route_reason::sole;
    return route;
  }

  // 4. Nothing names one viewer. By default that means "deliver nowhere":
  // broadcasting put every session's clipboard into every other session's OS
  // clipboard, which is the cross-talk this route exists to prevent. A host
  // that explicitly wants the old fan-out keeps it with allow_broadcast.
  if (!allow_broadcast) {
    route.reason = clipboard_route_reason::no_target;
    return route;
  }
  route.targets.reserve(live.size());
  for (const clipboard_route_session* session : live) {
    route.targets.push_back(session->id);
  }
  active = *std::max_element(
      live.begin(), live.end(),
      [](const clipboard_route_session* left,
         const clipboard_route_session* right) {
        return more_recent(*right, *left);
      });
  route.active = active != nullptr ? active->id : 0;
  route.reason = clipboard_route_reason::broadcast;
  return route;
}

std::string clipboard_text_key(const std::string& hash, std::size_t bytes) {
  return "text:" + hash + ":" + std::to_string(bytes);
}

std::string clipboard_files_key(const std::string& signature) {
  return "files:" + signature;
}

const char* clipboard_echo_direction_name(clipboard_echo_direction direction) {
  switch (direction) {
    case clipboard_echo_direction::delivered:
      return "delivered";
    case clipboard_echo_direction::received:
      return "received";
  }
  return "delivered";
}

clipboard_echo_table::clipboard_echo_table(uint64_t ttl_ms,
                                           std::size_t max_entries)
    : ttl_ms_(ttl_ms), max_entries_(max_entries == 0 ? 1 : max_entries) {}

void clipboard_echo_table::prune_locked(uint64_t now_ms) {
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [now_ms, this](const entry& item) {
                                  return older_than(now_ms, item.at, ttl_ms_);
                                }),
                 entries_.end());
}

void clipboard_echo_table::remember(uint32_t session_id, std::string key,
                                    clipboard_echo_direction direction,
                                    uint64_t now_ms) {
  if (key.empty()) return;
  std::lock_guard<std::mutex> lock(mutex_);
  prune_locked(now_ms);
  const auto existing =
      std::find_if(entries_.begin(), entries_.end(),
                   [session_id, &key](const entry& item) {
                     return item.session_id == session_id && item.key == key;
                   });
  if (existing != entries_.end()) entries_.erase(existing);
  entry item;
  item.session_id = session_id;
  item.key = std::move(key);
  item.direction = direction;
  item.at = now_ms;
  entries_.push_back(std::move(item));
  while (entries_.size() > max_entries_) entries_.pop_front();
}

bool clipboard_echo_table::suppress(uint32_t session_id, const std::string& key,
                                    uint64_t now_ms) {
  if (key.empty()) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  prune_locked(now_ms);
  return std::any_of(entries_.begin(), entries_.end(),
                     [session_id, &key](const entry& item) {
                       return item.session_id == session_id && item.key == key;
                     });
}

void clipboard_echo_table::forget_session(uint32_t session_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [session_id](const entry& item) {
                                  return item.session_id == session_id;
                                }),
                 entries_.end());
}

void clipboard_echo_table::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
}

std::size_t clipboard_echo_table::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.size();
}

}  // namespace molink
