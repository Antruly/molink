// Host-only outbound policy: loopback self-connections stay allowed, every
// other target is refused with the documented error code and log fields.
// Pure logic plus the diagnostic logger; no libuvcpp and no sockets.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

#include "diag_log.h"
#include "net/outbound_policy.h"

namespace {

namespace fs = std::filesystem;

int failures = 0;
int checks = 0;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

fs::path make_temp_dir(const char* label) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path dir = fs::temp_directory_path() /
                       (std::string("molink-outbound-") + label + "-" +
                        std::to_string(stamp));
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  return dir;
}

}  // namespace

int main() {
  using molink::outbound_verdict;

  // Inject the logger so the test never writes into the developer's real
  // diagnostics directory.
  const fs::path dir = make_temp_dir("log");
  molink::diag_log log(dir.string(), true);

  // host_only off: the old behaviour, every target keeps working.
  expect(molink::gate_outbound_session(false, "192.168.1.20", log) ==
             outbound_verdict::allow,
         "a normal install may dial a LAN address");
  expect(molink::gate_outbound_session(false, "127.0.0.1", log) ==
             outbound_verdict::allow,
         "a normal install may dial loopback");
  expect(molink::gate_outbound_session(false, "ABCD-EFGH-J", log) ==
             outbound_verdict::allow,
         "a normal install may dial a device id");

  // host_only on: only the loopback self-connection survives.
  expect(molink::gate_outbound_session(true, "192.168.1.20", log) ==
             outbound_verdict::host_only,
         "host_only refuses a LAN address");
  expect(molink::gate_outbound_session(true, "10.0.0.5", log) ==
             outbound_verdict::host_only,
         "host_only refuses a private address");
  expect(molink::gate_outbound_session(true, "ABCD-EFGH-J", log) ==
             outbound_verdict::host_only,
         "host_only refuses a directory device id");
  expect(molink::gate_outbound_session(true, "example.com", log) ==
             outbound_verdict::host_only,
         "host_only refuses a host name");
  expect(molink::gate_outbound_session(true, "127.0.0.1", log) ==
             outbound_verdict::allow,
         "host_only allows the loopback self-connection");
  expect(molink::gate_outbound_session(true, " 127.0.0.1 ", log) ==
             outbound_verdict::allow,
         "host_only trims the dialled address");
  expect(molink::gate_outbound_session(true, "::1", log) ==
             outbound_verdict::allow,
         "host_only allows IPv6 loopback");
  expect(molink::gate_outbound_session(true, "[::1]", log) ==
             outbound_verdict::allow,
         "host_only allows a bracketed IPv6 loopback");
  expect(molink::gate_outbound_session(true, "127.0.0.2", log) ==
             outbound_verdict::host_only,
         "host_only does not widen the loopback exception");

  expect(std::string(molink::kHostOnlyErrorCode) == "host_only_mode",
         "the close reason says host_only_mode");

  // The refusal line carries the fields the log contract names.
  const std::string tail = log.tail(20);
  expect(contains(tail, "session.outbound_rejected"),
         "the refusal is logged under session.outbound_rejected");
  expect(contains(tail, "reason=host_only"),
         "the refusal log carries reason=host_only");
  expect(contains(tail, "target=192.168.1.20"),
         "the refusal log carries the target");
  std::error_code ec;
  fs::remove_all(dir, ec);

  if (failures != 0) {
    std::fprintf(stderr, "%d of %d checks failed\n", failures, checks);
    return 1;
  }
  std::printf("outbound_policy: %d checks passed\n", checks);
  return 0;
}
