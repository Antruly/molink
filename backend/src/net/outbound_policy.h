#pragma once

#include <string>

#include "diag_log.h"

namespace molink {

/// Error code handed back to a caller whose outbound session was refused.
/// The WebSocket proxies close with this as the close reason; a front end
/// must treat it as a policy answer, not as a broken link.
inline constexpr char kHostOnlyErrorCode[] = "host_only_mode";

enum class outbound_verdict { allow, host_only };

/// True when @p host is the literal loopback address this machine may still
/// dial in host_only mode (the "view this screen" path). Only the address
/// actually being dialled is inspected; a source address supplied by the
/// caller is never trusted.
bool is_loopback_target(const std::string& host);

/// The single gate for every session this core opens as the controlling side:
/// /ws/connect (device-ID ICE proxy) and /ws/direct (typed address). With
/// @p host_only false this keeps the old behaviour. With it true, only a
/// loopback target is permitted and a refusal is logged to @p log (the
/// process-wide logger in production, an injected one in tests).
outbound_verdict gate_outbound_session(bool host_only,
                                       const std::string& target,
                                       diag_log& log = diag());

/// Writes `session.outbound_rejected reason=host_only target=<target>`.
/// Exposed so the refusal fields are covered without a live WebSocket.
void log_outbound_rejected(diag_log& log, const std::string& target);

}  // namespace molink
