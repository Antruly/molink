// Service-name isolation is pure string mapping, so it is pinned on every
// host: the full product must stay at its pre-isolation location, while another
// service (the host-only "moLinkHost") must get its own directory and named
// objects so the two installs cannot collide.
#include <cstdio>
#include <string>

#include "service_paths.h"

namespace {

using molink::service_paths::global_object_name;
using molink::service_paths::is_legacy_service;
using molink::service_paths::paths;
using molink::service_paths::resolve;

int checks = 0;
int failures = 0;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

void expect_eq(const std::wstring& actual, const std::wstring& expected,
               const char* description) {
  ++checks;
  if (actual == expected) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n  expected: %ls\n  actual:   %ls\n",
               description, expected.c_str(), actual.c_str());
}

bool starts_with(const std::wstring& value, const std::wstring& prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

int main() {
  const std::wstring program_data = L"C:\\ProgramData";

  // 1. The default service is byte-for-byte the pre-isolation layout.
  expect(is_legacy_service(L"moLinkSession"), "moLinkSession is legacy");
  expect(is_legacy_service(L""), "an empty name defaults to legacy");
  expect(!is_legacy_service(L"moLinkHost"), "moLinkHost is isolated");

  const paths legacy = resolve(program_data, L"moLinkSession");
  expect_eq(legacy.data_dir, L"C:\\ProgramData\\moLink\\service",
            "moLinkSession keeps %ProgramData%\\moLink\\service");
  expect_eq(legacy.service_log,
            L"C:\\ProgramData\\moLink\\service\\service.log",
            "legacy service.log stays put");
  expect_eq(legacy.helper_log,
            L"C:\\ProgramData\\moLink\\service\\helper.log",
            "legacy helper.log stays put");
  expect_eq(legacy.core_path_hint,
            L"C:\\ProgramData\\moLink\\service\\core.path",
            "legacy core.path stays put");
  expect_eq(legacy.core_pid, L"C:\\ProgramData\\moLink\\service\\core.pid",
            "legacy core.pid stays put");
  expect_eq(legacy.supervisor_mutex, L"Global\\moLinkCoreSupervisor",
            "legacy supervisor mutex name is unchanged");
  expect_eq(legacy.helper_stop_event, L"Global\\moLinkHelperStop",
            "legacy helper stop event is unchanged");
  expect_eq(legacy.helper_detach_event, L"Global\\moLinkHelperDetach",
            "legacy helper detach event is unchanged");

  // 2. Another service gets %ProgramData%\<SVC>\service and names that carry
  //    the service name.
  const paths host = resolve(program_data, L"moLinkHost");
  expect_eq(host.data_dir, L"C:\\ProgramData\\moLinkHost\\service",
            "moLinkHost gets its own service directory");
  expect_eq(host.service_log,
            L"C:\\ProgramData\\moLinkHost\\service\\service.log",
            "moLinkHost service.log is in its own directory");
  expect_eq(host.helper_log,
            L"C:\\ProgramData\\moLinkHost\\service\\helper.log",
            "moLinkHost helper.log is in its own directory");
  expect_eq(host.core_path_hint,
            L"C:\\ProgramData\\moLinkHost\\service\\core.path",
            "moLinkHost core.path is in its own directory");
  expect_eq(host.core_pid,
            L"C:\\ProgramData\\moLinkHost\\service\\core.pid",
            "moLinkHost core.pid is in its own directory");
  expect_eq(host.supervisor_mutex, L"Global\\moLinkHostCoreSupervisor",
            "moLinkHost supervisor mutex carries the service name");
  expect_eq(host.helper_stop_event, L"Global\\moLinkHostHelperStop",
            "moLinkHost stop event carries the service name");
  expect_eq(host.helper_detach_event, L"Global\\moLinkHostHelperDetach",
            "moLinkHost detach event carries the service name");
  expect(host.supervisor_mutex.find(L"moLinkHost") != std::wstring::npos,
         "the non-default object name contains the service name");
  expect(starts_with(host.supervisor_mutex, L"Global\\"),
         "objects live in the Global namespace");

  // 3. The two installations share nothing that mattered before: directory,
  //    logs, pid/hint files and every named object differ.
  expect(legacy.data_dir != host.data_dir,
         "the two services use different directories");
  expect(legacy.service_log != host.service_log,
         "the two services write different service logs");
  expect(legacy.helper_log != host.helper_log,
         "the two services write different helper logs");
  expect(legacy.core_pid != host.core_pid,
         "the two services do not share core.pid");
  expect(legacy.core_path_hint != host.core_path_hint,
         "the two services do not share core.path");
  expect(legacy.supervisor_mutex != host.supervisor_mutex,
         "the two helpers cannot take the same supervisor mutex");
  expect(legacy.helper_stop_event != host.helper_stop_event,
         "one service cannot stop the other's helper");
  expect(legacy.helper_detach_event != host.helper_detach_event,
         "one service cannot detach the other's helper");

  // 4. Every non-default service is distinct from every other one.
  const paths alpha = resolve(program_data, L"moLinkAlpha");
  const paths beta = resolve(program_data, L"moLinkBeta");
  expect(alpha.data_dir != beta.data_dir,
         "two custom services use different directories");
  expect(alpha.supervisor_mutex != beta.supervisor_mutex,
         "two custom services use different mutexes");
  expect(global_object_name(L"moLinkAlpha", L"CoreSupervisor") ==
             alpha.supervisor_mutex,
         "global_object_name matches the resolved mutex");
  expect(global_object_name(L"moLinkSession", L"CoreSupervisor") ==
             L"Global\\moLinkCoreSupervisor",
         "the legacy prefix is derived from the default service name");

  // 5. Inputs are normalised: trailing separators and an empty ProgramData.
  const paths trailing = resolve(L"C:\\ProgramData\\", L"moLinkHost");
  expect_eq(trailing.data_dir, host.data_dir,
            "a trailing separator does not change the directory");
  const paths missing = resolve(L"", L"moLinkHost");
  expect(missing.data_dir.empty(), "an empty ProgramData yields no data dir");
  expect_eq(missing.supervisor_mutex, L"Global\\moLinkHostCoreSupervisor",
            "object names do not depend on ProgramData");
  expect(missing.service_name == L"moLinkHost",
         "the service name survives an empty ProgramData");

  std::printf("service_paths: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
