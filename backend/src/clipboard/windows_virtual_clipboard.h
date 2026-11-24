#pragma once

#if defined(_WIN32)

#include <objidl.h>

#include <memory>

#include "clipboard/virtual_file_stream.h"

namespace molink {

/// Builds the OLE data object Explorer reads when a paste starts. The caller
/// owns the returned reference. Exposed so the unit test can exercise the
/// formats and streams without replacing the developer's clipboard.
HRESULT create_virtual_file_data_object(
    std::shared_ptr<virtual_stream_source> source, IDataObject** out);

/// True while this process's virtual-file data object is still the clipboard
/// owner. The Windows clipboard watcher asks before enumerating: describing
/// our own offer would send the viewer's files straight back to the host.
bool windows_virtual_clipboard_owns_clipboard();

}  // namespace molink

#endif  // _WIN32
