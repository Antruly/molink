#!/usr/bin/env python3
"""Every source of the moLink-native target must be in the MinGW build lists.

The Windows shell is built three ways: the CMake target, the Win7 x86 script and
the release workflow's MinGW step. Only the CMake target knows its sources, so a
new file added there compiles on MSVC and silently breaks the other two at link
time — which has now happened four times (preset_message, clipboard_sync,
file_panel_logic, adaptive_quality).

Run from the repository root; exits non-zero and names the missing files.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CMAKE = ROOT / "shell/windows/CMakeLists.txt"
LISTS = [
    ROOT / "packaging/windows/build-win7-x86.ps1",
    ROOT / ".github/workflows/release-desktop.yml",
]


def native_sources() -> list[str]:
    text = CMAKE.read_text(encoding="utf-8")
    match = re.search(r"add_executable\(moLink-native[^)]*\)", text, re.S)
    if not match:
        sys.exit("could not find the moLink-native target")
    names = []
    for line in match.group(0).splitlines():
        line = line.strip()
        if line.startswith("#") or not line:
            continue
        for name in re.findall(r"([\w-]+\.(?:cpp|c))", line):
            names.append(name)
    return names


def main() -> int:
    sources = native_sources()
    if not sources:
        sys.exit("the moLink-native target lists no sources")
    failures = []
    for path in LISTS:
        text = path.read_text(encoding="utf-8")
        missing = [name for name in sources if name not in text]
        if missing:
            failures.append((path.relative_to(ROOT), missing))
    if failures:
        for path, missing in failures:
            print(f"{path}: missing {', '.join(missing)}")
        return 1
    print(f"native build lists agree ({len(sources)} sources, {len(LISTS)} lists)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())