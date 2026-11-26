// Service-name -> filesystem / named-object mapping.
//
// The Windows service and its helper used to hard-code %ProgramData%\moLink and
// the Global\moLink* objects. That made the full product ("moLinkSession") and
// the host-only product ("moLinkHost") overwrite each other's binaries, logs and
// supervisor mutex when both were installed. Everything is now derived from the
// service name, and this header keeps the derivation free of Win32 so it can be
// unit-tested on any host.
//
// Compatibility rule: the default service, "moLinkSession", keeps the exact
// paths and object names used before this isolation, so upgrading the full
// product does not orphan its data directory or race an already-running helper.
// Only other service names get the isolated locations.
//
// Backslashes are used literally because the result is a Windows path or a
// Windows kernel object name, even when this code is compiled on Linux for the
// unit tests.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace molink {
namespace service_paths {

/// The full product's service. Missing/empty names are treated as this one so
/// a manually started helper behaves like the pre-isolation helper.
inline constexpr wchar_t kDefaultServiceName[] = L"moLinkSession";

/// The full product keeps the historical web port. Any other service (the
/// host-only "moLinkHost") must not compete with it for 28418, so its core
/// starts on a port from this isolated block instead.
inline constexpr int kLegacyPort = 28418;
inline constexpr int kFirstIsolatedPort = 28419;
inline constexpr int kLastIsolatedPort = 28430;

struct paths {
  /// Service name this mapping was derived from (empty input -> default).
  std::wstring service_name;
  /// ProgramData root as passed in, without trailing separators.
  std::wstring program_data;
  /// %ProgramData%\moLink\service (legacy) or %ProgramData%\<SVC>\service.
  std::wstring data_dir;
  std::wstring service_log;
  std::wstring helper_log;
  std::wstring core_pid;
  std::wstring core_path_hint;
  /// Port the helper passed to the core it launched (`core.port`). The UI
  /// reads this instead of assuming 28418: the host-only product's core runs
  /// on an isolated port while the full product owns 28418.
  std::wstring core_port;
  /// Starting web port for this service (28418 for the full product, a
  /// stable port from the isolated block otherwise). The helper still probes
  /// forward from here when the port is already taken.
  int default_port = kLegacyPort;
  /// Global\<prefix>CoreSupervisor (mutex held by the helper).
  std::wstring supervisor_mutex;
  /// Global\<prefix>HelperStop / Global\<prefix>HelperDetach.
  std::wstring helper_stop_event;
  std::wstring helper_detach_event;
};

/// True when `service_name` must use the pre-isolation locations. Empty names
/// are the default service too: the helper's `--name` is optional.
bool is_legacy_service(std::wstring_view service_name);

/// The name inserted into Global\ object names: "moLink" for the legacy
/// service, otherwise the service name itself. The legacy prefix is what makes
/// `Global\moLinkCoreSupervisor` stay unchanged for moLinkSession while
/// moLinkHost gets `Global\moLinkHostCoreSupervisor`.
std::wstring object_prefix(std::wstring_view service_name);

/// `Global\<object_prefix(service_name)><suffix>`.
std::wstring global_object_name(std::wstring_view service_name,
                                std::wstring_view suffix);

/// The default web port for `service_name`: 28418 for the legacy full
/// product, otherwise a stable port in [kFirstIsolatedPort,
/// kLastIsolatedPort]. The mapping is case-insensitive and deterministic, so
/// two installations of the same service always start at the same port.
int default_port_for(std::wstring_view service_name);

/// True when `core_path` is exactly `<directory>\molink-core.exe`.
///
/// This is a pure string comparison (case-insensitive, either slash accepted)
/// so the helper can decide whether a discovered core belongs to the service
/// that is about to adopt it without opening the process. It lives here, next
/// to the service-name mapping, so it can be unit-tested on any host.
bool is_core_path_in_dir(std::wstring_view core_path,
                         std::wstring_view directory);

/// Derive every path and object name. An empty `program_data` leaves the
/// path fields empty (callers must not fall back to a relative path) but the
/// Global\ object names are still filled in.
paths resolve(std::wstring_view program_data, std::wstring_view service_name);

}  // namespace service_paths
}  // namespace molink
