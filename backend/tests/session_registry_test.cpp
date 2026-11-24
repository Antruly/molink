// Connection budgets and per-session capabilities. A fake transport keeps the
// test free of libuvcpp and sockets.
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "session_registry.h"

namespace {

using namespace molink;

int checks = 0;
int failures = 0;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

struct fake_transport final : session_transport {
  explicit fake_transport(std::string address) : address(std::move(address)) {}

  const char* name() const override { return "fake"; }
  std::string peer_ip() const override { return address; }
  unsigned int peer_port() const override { return 1234; }
  bool is_open() const override { return true; }
  bool send_text(const std::string&) override { return true; }
  bool send_binary(const uint8_t*, std::size_t) override { return true; }
  void close(int, const std::string&) override {}
  void set_handlers(text_handler, binary_handler, close_handler) override {}

  std::string address;
};

std::shared_ptr<session_transport> endpoint(const std::string& address) {
  return std::make_shared<fake_transport>(address);
}

void test_per_ip_limit() {
  session_registry registry;
  for (std::size_t i = 0; i < session_registry::kMaxUnauthenticatedPerIp; ++i) {
    expect(registry.add(endpoint("10.0.0.1")) != nullptr,
           "per-IP budget admits the first 16");
  }
  session_registry::add_status status = session_registry::add_status::ok;
  expect(registry.add(endpoint("10.0.0.1"), &status) == nullptr,
         "the 17th connection from one address is refused");
  expect(status ==
             session_registry::add_status::per_ip_unauthenticated_limit,
         "refusal names the per-IP limit");
  expect(registry.add(endpoint("10.0.0.2")) != nullptr,
         "another address still connects");
}

void test_global_limit() {
  session_registry registry;
  const char* addresses[] = {"10.1.0.1", "10.1.0.2", "10.1.0.3", "10.1.0.4"};
  std::size_t added = 0;
  for (const char* address : addresses) {
    for (std::size_t i = 0; i < session_registry::kMaxUnauthenticatedPerIp;
         ++i) {
      if (registry.add(endpoint(address)) != nullptr) ++added;
    }
  }
  expect(added == session_registry::kMaxUnauthenticated,
         "the global budget fills at 64");
  session_registry::add_status status = session_registry::add_status::ok;
  expect(registry.add(endpoint("10.1.0.9"), &status) == nullptr,
         "the 65th unauthenticated connection is refused");
  expect(status ==
             session_registry::add_status::global_unauthenticated_limit,
         "refusal names the global limit");
}

void test_ipv4_mapped_counts_as_same_peer() {
  session_registry registry;
  for (std::size_t i = 0; i < session_registry::kMaxUnauthenticatedPerIp; ++i) {
    expect(registry.add(endpoint("::ffff:10.2.0.7")) != nullptr,
           "mapped address admitted");
  }
  expect(registry.add(endpoint("10.2.0.7")) == nullptr,
         "plain IPv4 and its mapped form share one budget");
}

void test_removal_frees_budget() {
  session_registry registry;
  std::shared_ptr<session_info> first;
  for (std::size_t i = 0; i < session_registry::kMaxUnauthenticatedPerIp; ++i) {
    auto added = registry.add(endpoint("10.3.0.1"));
    if (i == 0) first = added;
  }
  expect(registry.add(endpoint("10.3.0.1")) == nullptr,
         "budget is full before removal");
  registry.remove(first->id);
  expect(registry.add(endpoint("10.3.0.1")) != nullptr,
         "removal returns the slot");
}

void test_authenticated_limit() {
  session_registry registry;
  std::vector<std::shared_ptr<session_info>> sessions;
  // Distinct addresses keep the per-IP unauthenticated budget out of the way.
  for (std::size_t i = 0; i < session_registry::kMaxAuthenticated + 1; ++i) {
    const std::string address = "10.4." + std::to_string(i / 200) + "." +
                                std::to_string(i % 200 + 1);
    auto session = registry.add(endpoint(address));
    expect(session != nullptr, "authenticated-limit fixture connects");
    if (session) sessions.push_back(session);
  }
  for (std::size_t i = 0; i < session_registry::kMaxAuthenticated; ++i) {
    expect(registry.set_authenticated(sessions[i]),
           "the first 32 sessions authenticate");
  }
  expect(!registry.set_authenticated(sessions.back()),
         "the 33rd authenticated session is refused");
  expect(registry.has_authenticated(), "authenticated sessions are visible");
  // An already-authenticated session can be marked again without consuming a
  // second slot.
  expect(registry.set_authenticated(sessions[0]),
         "re-marking an authenticated session succeeds");
}

void test_capability_override() {
  session_registry registry;
  auto session = registry.add(endpoint("10.5.0.1"));
  expect(session != nullptr, "session for the capability test");
  expect(registry.has_capability(session->id, capability::screen),
         "a new session starts with the full mask");
  expect(registry.has_capability(session->id, capability::input),
         "full mask includes input");
  const uint32_t view_only = capabilities_for_policy("view_only");
  expect(registry.set_capabilities(session->id, view_only),
         "per-session override applies");
  expect(registry.has_capability(session->id, capability::screen),
         "view-only keeps screen");
  expect(!registry.has_capability(session->id, capability::input),
         "view-only removes input");
  expect(!registry.has_capability(session->id, capability::files_write),
         "view-only removes file writes");
  expect(registry.capabilities(session->id) == view_only,
         "capability mask is readable");
  expect(!registry.set_capabilities(session->id + 100, view_only),
         "an unknown session is not changed");
  expect(!registry.has_capability(session->id + 100, capability::screen),
         "an unknown session has no capabilities");
  const std::vector<session_info> snapshot = registry.snapshot();
  expect(snapshot.size() == 1 && snapshot[0].capabilities == view_only,
         "snapshot carries the per-session mask");
}

}  // namespace

int main() {
  test_per_ip_limit();
  test_global_limit();
  test_ipv4_mapped_counts_as_same_peer();
  test_removal_frees_budget();
  test_authenticated_limit();
  test_capability_override();
  if (failures == 0) {
    std::printf("session_registry: %d checks passed\n", checks);
    return 0;
  }
  std::fprintf(stderr, "session_registry: %d/%d checks failed\n", failures,
               checks);
  return 1;
}
