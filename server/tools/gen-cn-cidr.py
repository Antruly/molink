#!/usr/bin/env python3
"""Generate server/src/security/cn_cidr_generated.inc from APNIC stats.

The server binary embeds this table so a self-hosted deployment does not need
MaxMind or any other runtime geo database. Refresh it periodically:

    python3 -I server/tools/gen-cn-cidr.py
    python3 -I server/tools/gen-cn-cidr.py --check   # CI/manual drift check

The source is the public APNIC delegated-statistics file. Only IPv4 mainland
China allocations are included; IPv6 addresses are classified as overseas by
the policy layer (see security/ip.cpp).
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import ipaddress
import pathlib
import sys
import urllib.request

SOURCE_URL = "https://ftp.apnic.net/stats/apnic/delegated-apnic-latest"
DEFAULT_OUTPUT = (
    pathlib.Path(__file__).resolve().parent.parent
    / "src"
    / "security"
    / "cn_cidr_generated.inc"
)


def download(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "molink-cidr/1"})
    with urllib.request.urlopen(request, timeout=180) as response:
        return response.read()


def parse_ranges(data: bytes) -> list[tuple[int, int]]:
    ranges: list[tuple[int, int]] = []
    for line in data.decode("utf-8", "replace").splitlines():
        parts = line.split("|")
        if len(parts) < 7 or parts[1] != "CN" or parts[2] != "ipv4":
            continue
        if parts[6] not in ("allocated", "assigned"):
            continue
        try:
            start = int(ipaddress.IPv4Address(parts[3]))
            count = int(parts[4])
        except ValueError:
            continue
        if count <= 0:
            continue
        ranges.append((start, start + count - 1))

    ranges.sort()
    merged: list[tuple[int, int]] = []
    for start, end in ranges:
        if merged and start <= merged[-1][1] + 1:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((start, end))
    return merged


def render(data: bytes) -> str:
    ranges = parse_ranges(data)
    digest = hashlib.sha256(data).hexdigest()
    today = datetime.date.today().isoformat()
    lines = [
        "// Generated mainland-China IPv4 ranges. Do not edit by hand.",
        "//",
        f"// Source: {SOURCE_URL}",
        f"// Snapshot: {today}",
        f"// Source SHA-256: {digest}",
        "// Regenerate with: python3 -I server/tools/gen-cn-cidr.py",
        "",
        "static constexpr cn_range kCnRanges[] = {",
    ]
    for start, end in ranges:
        lines.append(f"    {{0x{start:08x}u, 0x{end:08x}u}},")
    lines.append("};")
    lines.append(
        "static constexpr std::size_t kCnRangeCount = "
        "sizeof(kCnRanges) / sizeof(kCnRanges[0]);"
    )
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path, default=DEFAULT_OUTPUT)
    parser.add_argument(
        "--check",
        action="store_true",
        help="exit non-zero when the checked-in table differs from the source",
    )
    args = parser.parse_args()

    try:
        data = download(SOURCE_URL)
    except Exception as error:  # noqa: BLE001 - report the URL failure verbatim
        print(f"failed to download APNIC stats: {error}", file=sys.stderr)
        return 1

    rendered = render(data)
    if args.check:
        current = args.output.read_text(encoding="utf-8") if args.output.exists() else ""
        if current != rendered:
            print(f"{args.output} is out of date", file=sys.stderr)
            return 1
        print(f"{args.output} is up to date")
        return 0

    args.output.write_text(rendered, encoding="utf-8")
    count = rendered.count("{0x")
    print(f"wrote {args.output} ({count} ranges)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
