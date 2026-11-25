#include "net/outbound_policy.h"

#include <string>

namespace molink {
namespace {

std::string trim_copy(const std::string& value) {
  std::size_t start = 0;
  while (start < value.size() &&
         (value[start] == ' ' || value[start] == '\t' ||
          value[start] == '\r' || value[start] == '\n')) {
    ++start;
  }
  std::size_t end = value.size();
  while (end > start &&
         (value[end - 1] == ' ' || value[end - 1] == '\t' ||
          value[end - 1] == '\r' || value[end - 1] == '\n')) {
    --end;
  }
  return value.substr(start, end - start);
}

}  // namespace

bool is_loopback_target(const std::string& host) {
  std::string value = trim_copy(host);
  if (value == "127.0.0.1" || value == "::1") return true;
  // A bracketed IPv6 literal is what a page puts in a URL.
  if (value.size() > 2 && value.front() == '[' && value.back() == ']') {
    value = value.substr(1, value.size() - 2);
    return value == "::1";
  }
  return false;
}

void log_outbound_rejected(diag_log& log, const std::string& target) {
  log.log(diag_level::warn, "session.outbound_rejected", std::string(),
          {{"reason", "host_only"}, {"target", target}});
}

outbound_verdict gate_outbound_session(bool host_only,
                                       const std::string& target,
                                       diag_log& log) {
  if (!host_only) return outbound_verdict::allow;
  if (is_loopback_target(target)) return outbound_verdict::allow;
  log_outbound_rejected(log, target);
  return outbound_verdict::host_only;
}

}  // namespace molink
