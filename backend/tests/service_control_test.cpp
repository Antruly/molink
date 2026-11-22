// The service path staleness rule: a registered ImagePath is healthy only when
// it names the binary this build expects and that file still exists.
#include <cstdio>
#include <string>

#include "service_control.h"

namespace {

int g_failures = 0;

void expect(bool ok, const char* what) {
  if (ok) return;
  ++g_failures;
  std::fprintf(stderr, "FAIL: %s\n", what);
}

}  // namespace

int main() {
  using molink::service_status_is_stale;

  const std::string expected =
      "C:\\ProgramData\\moLink\\service\\molink-service.exe";

  // 1. The registered path matches and the binary is present: healthy.
  expect(!service_status_is_stale(expected, expected, true),
         "matching path + existing binary is not stale");

  // 2. The registered path matches but the binary is gone: stale.
  expect(service_status_is_stale(expected, expected, false),
         "matching path with a missing binary is stale");

  // 3. The registered path names a different binary: stale.
  expect(service_status_is_stale(
             "C:\\ProgramData\\moLink\\service\\old-service.exe", expected,
             true),
         "a different registered path is stale");

  // Registry ImagePath values may be quoted, and Windows paths are
  // case-insensitive: neither should be reported as stale.
  expect(!service_status_is_stale("\"" + expected + "\"", expected, true),
         "surrounding quotes are ignored");
  expect(!service_status_is_stale(
             "c:\\programdata\\molink\\service\\MOLINK-SERVICE.EXE", expected,
             true),
         "path comparison is case-insensitive");

  // No expectation means the platform cannot judge; an empty registration is
  // never healthy.
  expect(!service_status_is_stale("", "", false),
         "an empty expectation is never stale");
  expect(service_status_is_stale("", expected, true),
         "an empty registered path is stale");

  if (g_failures != 0) {
    std::fprintf(stderr, "%d service_control check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("service_control checks passed\n");
  return 0;
}
