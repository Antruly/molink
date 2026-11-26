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
  }

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
