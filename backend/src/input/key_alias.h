#pragma once

#include <string>

namespace molink {

/// Normalizes the aliases a browser or an older client may send for
/// `KeyboardEvent.code` values.
///
/// Browsers normally report `MetaLeft`/`MetaRight`, but some report the
/// legacy `OSLeft`/`OSRight`, and older moLink clients fell back to the
/// printable key name (`"Win"`, `"Super"`, `"超"`). A single lowercase letter
/// or digit is accepted too, because a client whose `event.code` was empty
/// used to send `event.key` unchanged.
///
/// The canonical spelling is returned for known aliases; anything else is
/// returned unchanged so the platform resolver can still recognise it.
std::string canonical_key_code(const std::string& code);

}  // namespace molink
