#pragma once

#include <string>

#include "capabilities.h"

namespace molink {

/// What a viewer opened a session for. A files/chat/terminal tab is an
/// independent session with its own page; it must never start capture,
/// subscribe to video or inject input, even though it authenticates with the
/// same host credential as the desktop tab.
///
/// Values are exact lowercase names. An absent, empty or unknown field keeps
/// the historical desktop session, so an older client is never mistaken for a
/// picture-less page.
inline std::string normalize_session_intent(const std::string& raw) {
  if (raw == "files" || raw == "chat" || raw == "terminal") {
    return raw;
  }
  return "desktop";
}

inline bool session_intent_is_desktop(const std::string& intent) {
  return intent.empty() || intent == "desktop";
}

/// Only a desktop page consumes the picture. Every other intent leaves the
/// capture alone and may not subscribe to a video clock.
inline bool intent_wants_video(const std::string& intent) {
  return session_intent_is_desktop(intent);
}

/// Alias kept for the welcome/capability code, which reads as "may this
/// session take the video path".
inline bool session_intent_streams_video(const std::string& intent) {
  return intent_wants_video(intent);
}

/// Forces the hello's `video_push` back off for a non-desktop intent.
inline bool session_intent_allows_push(const std::string& intent) {
  return intent_wants_video(intent);
}

/// Input injection belongs to the remote picture. The files/chat/terminal
/// pages have their own local inputs (path fields, text box, keyboard) and
/// must never drive the host's pointer or keyboard through the session.
inline bool intent_allows_input(const std::string& intent) {
  return intent_wants_video(intent);
}

/// A short stable name for logs and tests.
inline const char* session_intent_name(const std::string& intent) {
  if (intent == "files") return "files";
  if (intent == "chat") return "chat";
  if (intent == "terminal") return "terminal";
  return "desktop";
}

/// Removes the two capabilities a non-desktop page must never exercise.
/// Files, chat and clipboard bits are preserved.
inline uint32_t session_intent_capabilities(uint32_t capabilities,
                                            const std::string& intent) {
  if (session_intent_is_desktop(intent)) return capabilities;
  return capabilities &
         ~(capability_bit(capability::screen) |
           capability_bit(capability::input));
}

}  // namespace molink
