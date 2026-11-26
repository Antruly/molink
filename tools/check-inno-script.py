#!/usr/bin/env python3
"""Static checks for the host-only Inno Setup script.

Inno Setup only runs on Windows, so the Linux CI job cannot compile
packaging/windows/host-only.iss. This script checks the parts that can drift
silently:

  * the staged payload list agrees with build-host-only.ps1;
  * uninstall stops the service before uninstalling it;
  * the product uses a different AppId (and service/data names) than the full
    installer, so both can be installed side by side;
  * the service is started automatically (delayed-auto), not on demand;
  * uninstall tells the user that %ProgramData%\\moLinkHost is preserved.

Usage (from the repository root):
    python3 tools/check-inno-script.py packaging/windows/host-only.iss
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
ISS = ROOT / "packaging/windows/host-only.iss"
BUILD_PS1 = ROOT / "packaging/windows/build-host-only.ps1"
FULL_ISS = [ROOT / "packaging/windows/installer.iss",
            ROOT / "packaging/windows/win7-x86-installer.iss"]

GUID_RE = re.compile(r"^\{\{[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-"
                     r"[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\}$")


class Failure(Exception):
    pass


def read(path: pathlib.Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        try:
            label = path.relative_to(ROOT)
        except ValueError:
            label = path
        raise Failure(f"cannot read {label}: {exc}") from exc


def define(text: str, name: str) -> str:
    match = re.search(
        r"^\s*#define\s+" + re.escape(name) + r'\s+"([^"]*)"', text, re.M
    )
    if not match:
        raise Failure(f"missing #define {name}")
    return match.group(1)


def section(text: str, name: str) -> str:
    match = re.search(
        r"^\[" + re.escape(name) + r"\]\s*$(.*?)(?=^\[|\Z)",
        text,
        re.M | re.S,
    )
    return match.group(1) if match else ""


def payload_list(value: str) -> list[str]:
    return [item.strip() for item in value.replace(";", ",").split(",") if item.strip()]


def check_payload(iss: str, ps1: str) -> None:
    iss_payload = payload_list(define(iss, "HostPayload"))
    if not iss_payload:
        raise Failure("HostPayload is empty")
    match = re.search(r"\$HostPayload\s*=\s*@\((.*?)\)", ps1, re.S)
    if not match:
        raise Failure("build-host-only.ps1 has no $HostPayload array")
    ps_payload = re.findall(r"'([^']+)'", match.group(1))
    if iss_payload != ps_payload:
        raise Failure(
            "payload lists differ:\n"
            f"  host-only.iss:        {iss_payload}\n"
            f"  build-host-only.ps1:  {ps_payload}"
        )
    for required in ("moLink-host.exe", "molink-core.exe",
                     "molink-service.exe", "molink-helper.exe", "molink.ico"):
        if required not in iss_payload:
            raise Failure(f"HostPayload is missing {required}")
        if f"(Join-Path $Stage '{required}')" not in ps1:
            raise Failure(f"build-host-only.ps1 does not stage {required}")

    iss_web = define(iss, "HostWebDir")
    web_match = re.search(r"\$HostWebDir\s*=\s*'([^']+)'", ps1)
    if not web_match:
        raise Failure("build-host-only.ps1 has no $HostWebDir")
    if iss_web != web_match.group(1):
        raise Failure(
            f"web directory differs: iss={iss_web!r}, ps1={web_match.group(1)!r}"
        )
    if "Join-Path $Stage $HostWebDir" not in ps1:
        raise Failure("build-host-only.ps1 does not stage the web directory")

    files = section(iss, "Files")
    if "{#PkgDir}\\*" not in files:
        raise Failure("[Files] must install the whole staged tree ({#PkgDir}\\*)")
    if "recursesubdirs" not in files:
        raise Failure("[Files] must use recursesubdirs so web\\ is copied")

    if "index.html" not in ps1 or "FrontendDist" not in ps1:
        raise Failure("build-host-only.ps1 does not require the frontend build")


def check_identity(iss: str, full_scripts: list[str]) -> None:
    app_id = define(iss, "AppId")
    if not GUID_RE.match(app_id):
        raise Failure(f"AppId is not a GUID: {app_id!r}")
    for full_iss in full_scripts:
        if app_id == define(full_iss, "AppId"):
            raise Failure(
                "AppId is a full product GUID; the products would collide"
            )
    if define(iss, "AppName") != "moLink Host":
        raise Failure("AppName must be 'moLink Host'")
    if "DefaultDirName={autopf}\\moLinkHost" not in iss:
        raise Failure("DefaultDirName must be {autopf}\\moLinkHost")
    if not re.search(r"^MinVersion=6\.1sp1\s*$", iss, re.M):
        raise Failure("MinVersion must be 6.1sp1")
    if not re.search(r"^ArchitecturesAllowed=x64\s*$", iss, re.M):
        raise Failure("ArchitecturesAllowed must be x64")
    if not re.search(r"^ArchitecturesInstallIn64BitMode=x64\s*$", iss, re.M):
        raise Failure("ArchitecturesInstallIn64BitMode must be x64")

    # The full product uses moLinkSession; the host-only product must never
    # touch that service.
    for name in ("Run", "UninstallRun", "Code"):
        if "moLinkSession" in section(iss, name):
            raise Failure(f"[{name}] references the full product's service moLinkSession")


def check_install(iss: str) -> None:
    tasks = section(iss, "Tasks")
    if not re.search(r'Name:\s*"service"', tasks):
        raise Failure("[Tasks] has no 'service' task")
    if not re.search(r'Name:\s*"autostart"', tasks):
        raise Failure("[Tasks] has no 'autostart' task")
    if not re.search(r'Name:\s*"desktopicon"', tasks):
        raise Failure("[Tasks] has no 'desktopicon' task")

    run = section(iss, "Run")
    if "--install --name moLinkHost" not in run:
        raise Failure("[Run] does not run molink-service.exe --install --name moLinkHost")
    if "start= delayed-auto" not in run:
        raise Failure(
            "[Run] does not configure delayed-auto; demand start would not run "
            "before a user logs on"
        )
    if "Software\\Microsoft\\Windows\\CurrentVersion\\Run" not in section(iss, "Registry"):
        raise Failure("[Registry] has no HKCU Run entry for the autostart task")
    if 'ValueName: "moLinkHost"' not in section(iss, "Registry"):
        raise Failure("[Registry] Run value must be named moLinkHost")


def check_uninstall(iss: str) -> None:
    uninstall = section(iss, "UninstallRun")
    stop = uninstall.find("stop moLinkHost")
    remove = uninstall.find("--uninstall --name moLinkHost")
    if stop < 0:
        raise Failure("[UninstallRun] does not stop moLinkHost")
    if remove < 0:
        raise Failure("[UninstallRun] does not run --uninstall --name moLinkHost")
    if stop > remove:
        raise Failure("[UninstallRun] must stop moLinkHost before uninstalling it")

    # Killing the shared image name would also kill the full product's core.
    if re.search(r"taskkill[^\n]*/IM\s+molink-core\.exe", uninstall, re.I):
        raise Failure("[UninstallRun] must not taskkill the shared molink-core.exe image")
    if "core.pid" not in uninstall:
        raise Failure("[UninstallRun] does not terminate the supervised core via core.pid")

    if "{commonappdata}\\moLinkHost" not in iss:
        raise Failure("[Code] must name {commonappdata}\\moLinkHost when explaining data retention")
    if "保留" not in iss:
        raise Failure("the uninstall step must say that %ProgramData%\\moLinkHost is preserved")
    if re.search(r"DeleteFile|RemoveDir|DelTree", iss):
        raise Failure("host-only.iss must not delete the preserved data directory")


def check_x86(ps1: str, iss: str) -> None:
    if "ValidateSet('x64', 'x86')" not in ps1 and 'ValidateSet("x64", "x86")' not in ps1:
        raise Failure("build-host-only.ps1 must accept an -Arch parameter")
    if "x86" not in ps1:
        raise Failure("build-host-only.ps1 does not mention the unsupported x86 path")
    if "暂不支持" not in ps1 and "not supported" not in ps1.lower():
        raise Failure("build-host-only.ps1 must fail explicitly for x86")
    if "throw" not in ps1:
        raise Failure("build-host-only.ps1 must throw for x86 rather than continue")
    setup = section(iss, "Setup")
    if re.search(r"^ArchitecturesAllowed=.*x86", setup, re.M | re.I):
        raise Failure("host-only.iss must not advertise an x86 architecture")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "iss", nargs="?", default=str(ISS),
        help="Inno Setup script to check (default: packaging/windows/host-only.iss)",
    )
    args = parser.parse_args()

    iss_path = pathlib.Path(args.iss)
    if not iss_path.is_absolute():
        iss_path = (pathlib.Path.cwd() / iss_path).resolve()

    try:
        iss = read(iss_path)
        ps1 = read(BUILD_PS1)
        full_iss = [read(path) for path in FULL_ISS]
        check_payload(iss, ps1)
        check_identity(iss, full_iss)
        check_install(iss)
        check_uninstall(iss)
        check_x86(ps1, iss)
    except Failure as exc:
        print(f"host-only installer check failed: {exc}", file=sys.stderr)
        return 1

    print("host-only installer check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
