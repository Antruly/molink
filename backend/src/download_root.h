#pragma once

#include <ctime>
#include <string>

namespace molink {

/// Default storage root for files received from a remote device.
///
/// Windows: `%USERPROFILE%\moLink` (falling back to `%HOMEDRIVE%%HOMEPATH%`).
/// Linux/macOS: `$HOME/moLink`.
///
/// This is a *default*, not a hidden constant: the settings screen stores a
/// user-chosen `download_root` and every new transfer resolves it at start.
/// Nothing here creates a directory; callers create the deepest path only when
/// the first byte is about to land, so installing the product leaves no empty
/// folder behind.
std::string default_download_root();

/// The configured root, or the platform default when @p configured is empty.
std::string resolve_download_root(const std::string& configured);

enum class download_root_error {
  none,
  empty,
  relative,
  system_directory,
  install_directory,
  not_directory,
  not_writable,
};

/// Stable machine-readable reason, suitable for a JSON `reason` field. Never
/// empty for an error and always "ok" for success.
const char* download_root_error_name(download_root_error error);

struct download_root_check {
  bool ok = false;
  download_root_error error = download_root_error::none;
  /// English explanation; the page localizes from `error` instead of showing
  /// this directly, but logs and `curl` output use it.
  std::string message;
};

/// Validates a user-chosen storage root before it is persisted.
///
/// Accepted: an absolute path the process can write to (the directory itself
/// or its nearest existing parent). Unicode and spaces are fine.
///
/// Refused: empty, relative, a filesystem root, a system directory
/// (`/usr`, `/etc`, `/bin`, `C:\Windows`, ...), the program's install
/// directory, an existing non-directory, and an unwritable location.
///
/// The check never creates anything.
download_root_check validate_download_root(const std::string& root);

/// Cleans a remote device id for use as one path component.
///
/// `\ / : * ? " < > |` and control characters become `_`; everything else,
/// including Chinese and the dots in an IP address, is preserved. The result
/// is UTF-8 safe, at most 64 bytes, and never `.` or `..`. An id that sanitizes
/// to nothing becomes `unknown`.
std::string sanitize_device_component(const std::string& component);

/// The local-timezone date as `YYYY-MM-DD`.
std::string local_date_string(std::time_t when = std::time(nullptr));

/// `<root>/<sanitized device id>/<YYYY-MM-DD>`.
///
/// The date is sanitized too, so a malformed value cannot escape the root.
/// No directory is created.
std::string download_directory(const std::string& root,
                               const std::string& device_id,
                               const std::string& date);

/// `<root>/<sanitized device id>/<YYYY-MM-DD>/chat`, for chat attachments.
///
/// Chat is deliberately not wired yet; this is the single helper the chat
/// receive path will use so both kinds of files keep the same layout.
std::string chat_attachment_dir(const std::string& root,
                                const std::string& device_id,
                                const std::string& date);

/// `directory/name`, or `directory/<stem> (N)<ext>` when that already exists.
///
/// Only the last extension moves: `a.tar.gz` becomes `a.tar (1).gz`. Nothing
/// is overwritten and no directory is created.
std::string unique_path(const std::string& directory,
                        const std::string& name);

/// Directory containing this executable, or empty when it cannot be resolved.
std::string program_directory();

}  // namespace molink
