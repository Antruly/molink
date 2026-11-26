#include "service_paths.h"

namespace molink {
namespace service_paths {
namespace {

std::wstring stripped(std::wstring_view path) {
  while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) {
    path.remove_suffix(1);
  }
  return std::wstring(path);
}

std::wstring join(std::wstring_view directory, std::wstring_view name) {
  if (directory.empty()) return std::wstring();
  std::wstring out(directory);
  if (out.back() != L'\\' && out.back() != L'/') out.push_back(L'\\');
  out.append(name);
  return out;
}

wchar_t ascii_lower(wchar_t value) {
  return value >= L'A' && value <= L'Z' ? value - L'A' + L'a' : value;
}

/// Windows file names are case-insensitive, so compare the portability-safe
/// way here rather than dragging in a platform header.
bool equals_ignore_case(std::wstring_view left, std::wstring_view right) {
  if (left.size() != right.size()) return false;
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (ascii_lower(left[i]) != ascii_lower(right[i])) return false;
  }
  return true;
}

std::wstring normalized_separators(std::wstring_view path) {
  std::wstring out(path);
  for (wchar_t& ch : out) {
    if (ch == L'/') ch = L'\\';
  }
  return out;
}

}  // namespace

bool is_legacy_service(std::wstring_view service_name) {
  return service_name.empty() || service_name == kDefaultServiceName;
}

std::wstring object_prefix(std::wstring_view service_name) {
  // Same upgrade rule as the data directory: the full product's mutex and
  // events must keep their old names, because an old helper may still be
  // running while the new service binary starts. "moLink" + "CoreSupervisor"
  // is exactly the pre-isolation `Global\moLinkCoreSupervisor`.
  return is_legacy_service(service_name) ? std::wstring(L"moLink")
                                         : std::wstring(service_name);
}

std::wstring global_object_name(std::wstring_view service_name,
                                std::wstring_view suffix) {
  std::wstring name(L"Global\\");
  name += object_prefix(service_name);
  name.append(suffix);
  return name;
}

int default_port_for(std::wstring_view service_name) {
  if (is_legacy_service(service_name)) return kLegacyPort;
  // FNV-1a over the lower-cased service name. Only stability matters: the
  // helper probes forward if another service already owns the first choice.
  std::uint32_t hash = 2166136261u;
  for (wchar_t ch : service_name) {
    hash ^= static_cast<std::uint32_t>(ascii_lower(ch)) & 0xFFFFu;
    hash *= 16777619u;
  }
  const int span = kLastIsolatedPort - kFirstIsolatedPort + 1;
  return kFirstIsolatedPort +
         static_cast<int>(hash % static_cast<std::uint32_t>(span));
}

bool is_core_path_in_dir(std::wstring_view core_path,
                         std::wstring_view directory) {
  if (core_path.empty() || directory.empty()) return false;
  std::wstring dir(directory);
  // A trailing separator is common when the path was built by appending one;
  // strip it so "C:\Program Files\moLinkHost\" and "...\moLinkHost" match.
  while (dir.size() > 1 && (dir.back() == L'\\' || dir.back() == L'/')) {
    dir.pop_back();
  }
  if (dir.empty()) return false;
  std::wstring expected = dir;
  if (expected.back() != L'\\' && expected.back() != L'/') {
    expected.push_back(L'\\');
  }
  expected.append(L"molink-core.exe");
  return equals_ignore_case(normalized_separators(core_path),
                            normalized_separators(expected));
}

paths resolve(std::wstring_view program_data, std::wstring_view service_name) {
  paths result;
  result.service_name = is_legacy_service(service_name)
                            ? std::wstring(kDefaultServiceName)
                            : std::wstring(service_name);
  result.program_data = stripped(program_data);
  if (!result.program_data.empty()) {
    // The legacy product used %ProgramData%\moLink\service for both the
    // session service and its helper. Moving those files during an upgrade
    // would lose core.path/logs and could leave the old uninstaller pointing
    // at nothing, so only new service names get their own top-level directory.
    const std::wstring folder =
        result.service_name == kDefaultServiceName
            ? std::wstring(L"moLink")
            : result.service_name;
    result.data_dir =
        join(join(result.program_data, folder), std::wstring(L"service"));
    result.service_log = join(result.data_dir, L"service.log");
    result.helper_log = join(result.data_dir, L"helper.log");
    result.core_pid = join(result.data_dir, L"core.pid");
    result.core_path_hint = join(result.data_dir, L"core.path");
    result.core_port = join(result.data_dir, L"core.port");
  }
  result.default_port = default_port_for(service_name);

  // The object names do not depend on ProgramData: a service with an unset
  // environment still has to create and open its own events and mutex.
  result.supervisor_mutex =
      global_object_name(service_name, L"CoreSupervisor");
  result.helper_stop_event =
      global_object_name(service_name, L"HelperStop");
  result.helper_detach_event =
      global_object_name(service_name, L"HelperDetach");
  return result;
}

}  // namespace service_paths
}  // namespace molink
