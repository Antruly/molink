#include "input/key_alias.h"

#include <cctype>

namespace molink {
namespace {

struct Alias {
  const char* from;
  const char* to;
};

// Names a browser or an older client may use for the Windows/Command key.
const Alias kAliases[] = {
    {"OSLeft", "MetaLeft"},      {"OSRight", "MetaRight"},
    {"Win", "MetaLeft"},         {"WinLeft", "MetaLeft"},
    {"WinRight", "MetaRight"},   {"Super", "MetaLeft"},
    {"SuperLeft", "MetaLeft"},   {"SuperRight", "MetaRight"},
    {"Meta", "MetaLeft"},        {"OS", "MetaLeft"},
    {"Command", "MetaLeft"},     {"Cmd", "MetaLeft"},
    {"CommandLeft", "MetaLeft"}, {"CommandRight", "MetaRight"},
    {"超", "MetaLeft"},
};

}  // namespace

std::string canonical_key_code(const std::string& code) {
  for (const Alias& alias : kAliases) {
    if (code == alias.from) return alias.to;
  }
  if (code.size() == 1) {
    const unsigned char c = static_cast<unsigned char>(code[0]);
    if (c >= 'a' && c <= 'z') {
      return std::string("Key") + static_cast<char>(std::toupper(c));
    }
    if (c >= 'A' && c <= 'Z') return std::string("Key") + code;
    if (c >= '0' && c <= '9') return std::string("Digit") + code;
  }
  return code;
}

}  // namespace molink
