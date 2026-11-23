// Error routing for the native session tab.
//
// A tab has two feedback channels:
//   * the session banner, for failures that make the session itself unusable;
//   * a toast, for a failure that belongs to one feature (input, clipboard,
//     one file action). The session is still live, so it must never look like
//     a disconnect.
//
// The rules live here, away from the Win32 view, so the Linux tests and the
// shell agree on what "the link is broken" means.
#pragma once

#include <string>

namespace molink::native::alerts {

enum class tone { info, warn, error };

enum class surface { toast, banner };

struct route {
  surface where = surface::toast;
  tone level = tone::error;
  /// Only meaningful for the banner: whether reconnecting can help.
  bool reconnect = false;
};

/// Codes (or close reasons) that belong in the session banner. The task's
/// set is authoritative: a closed transport, the four credential failures,
/// and a failed handshake. `invalid_code` is the native shell's older name
/// for a rejected code, so it is treated as `invalid_token`.
bool is_session_error(const std::string& code);

/// Lower-case substring test used for close reasons that arrive as prose
/// instead of a code (`hello timeout`, `handshake failed`, ...).
bool looks_like_session_failure(const std::string& message);

/// Returns the toast/banner split and the toast tone for one failure.
route classify(const std::string& code, const std::string& message);

const char* tone_name(tone value);

}  // namespace molink::native::alerts
