#include "clipboard_pull.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace molink {
namespace {

namespace fs = std::filesystem;

clipboard_pull_result gone(uint64_t offset) {
  clipboard_pull_result result;
  result.status = clipboard_pull_status::gone;
  result.offset = offset;
  return result;
}

clipboard_pull_result read_error(uint64_t offset) {
  clipboard_pull_result result;
  result.status = clipboard_pull_status::read_error;
  result.offset = offset;
  return result;
}

}  // namespace

clipboard_pull_result read_clipboard_pull(
    const std::vector<clipboard_file>& current,
    const std::vector<clipboard_offer_entry>& offered, uint32_t id,
    uint64_t offset, uint64_t length, uint64_t max_chunk) {
  if (id == 0 || offered.empty() || id > offered.size()) return gone(offset);
  // The host published one list and the viewer is answering it. A different
  // count or a different name at this position means the clipboard moved on;
  // downloading whatever now sits at that index would silently paste the
  // wrong file.
  if (current.size() != offered.size()) return gone(offset);
  const std::size_t index = static_cast<std::size_t>(id - 1);
  if (index >= current.size()) return gone(offset);
  const clipboard_file& file = current[index];
  // An inner RDP virtual file has no path: its bytes come from the OLE data
  // object and the caller reads them through the clipboard backend. Anything
  // with neither a path nor a stream id cannot be read.
  const bool stream = file.stream_id != 0;
  if ((!stream && file.path.empty()) || file.name != offered[index].name) {
    return gone(offset);
  }
  // A rewritten RDP temporary file can keep its name but change size. The
  // viewer's staging entry was declared with the old size, so it cannot accept
  // the new bytes either.
  if (file.size != offered[index].size) return gone(offset);
  if (offset > file.size) return read_error(offset);

  const uint64_t available = file.size - offset;
  const uint64_t take = std::min({length, available, max_chunk});
  clipboard_pull_result result;
  result.offset = offset;
  result.length = take;
  result.stream = stream;
  result.status = clipboard_pull_status::ok;
  if (take == 0) return result;
  if (stream) return result;

  std::error_code ec;
  if (!fs::is_regular_file(fs::path(file.path), ec) || ec) {
    // RDP closes its redirected temporary file when the inner session drops.
    return gone(offset);
  }
  std::ifstream in(file.path, std::ios::binary);
  if (!in) return read_error(offset);
  in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!in) return read_error(offset);
  result.data.resize(static_cast<std::size_t>(take));
  in.read(reinterpret_cast<char*>(result.data.data()),
          static_cast<std::streamsize>(take));
  const std::streamsize got = in.gcount();
  if (got <= 0) {
    result.data.clear();
    return read_error(offset);
  }
  result.data.resize(static_cast<std::size_t>(got));
  return result;
}

}  // namespace molink
