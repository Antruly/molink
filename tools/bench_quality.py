#!/usr/bin/env python3
"""Measure the three quality presets against a live core.

Run this while the captured screen has some activity (moving window, video,
scrolling text). A static screen makes the bandwidth comparison meaningless,
although the frame-rate checks still work.

    MOLINK_CODE='the-verification-code' \
      python3 tools/bench_quality.py http://127.0.0.1:28418

If the core rejects web-client login, pass the session token printed by
`molink-core --print-ready` instead:

    python3 tools/bench_quality.py http://127.0.0.1:28418 \
      --token <ready-token>

The script sets each preset, requests a keyframe, then pulls frames at the
preset's frame interval for --seconds. It reports keyframe size, wire/painted
FPS, throughput, and 95th-percentile frame gap, and exits non-zero when a
threshold fails.

Thresholds are intentionally looser than the measured Linux/X11 numbers so
the tool is useful on slower hosts:

  * smooth bandwidth <= 70% of balanced
  * smooth keyframe  <= 70% of balanced
  * wire FPS: smooth >= 25, balanced >= 17, quality >= 13
  * p95 frame gap: smooth <= 100 ms, balanced <= 150 ms, quality <= 200 ms
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import statistics
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

try:
    import websockets
except ImportError:
    sys.exit("pip install websockets")

# mode -> (capture fps, pull interval seconds)
PROFILES = {
    "smooth": (30, 1 / 30),
    "balanced": (20, 1 / 20),
    "quality": (15, 1 / 15),
}


def http_json(base: str, path: str, token: str | None = None,
              method: str = "GET") -> tuple[int, dict]:
    request = urllib.request.Request(base + path, method=method)
    if token:
        request.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(request, timeout=15) as response:
            return response.status, json.loads(response.read() or b"{}")
    except urllib.error.HTTPError as error:
        return error.code, {}


def parse_frame(packet: bytes) -> dict | None:
    if len(packet) < 48 or packet[:4] != b"MOLK" or packet[5] != 2:
        return None
    count = struct.unpack_from("<H", packet, 36)[0]
    return {
        "bytes": len(packet),
        "tiles": count,
        "codec": packet[38],
        "keyframe": bool(packet[39] & 1),
    }


async def drain(ws, timeout: float = 0.01) -> None:
    try:
        while True:
            await asyncio.wait_for(ws.recv(), timeout)
    except asyncio.TimeoutError:
        return


async def keyframe_size(ws) -> tuple[int, float, int]:
    """Ask for a keyframe and return (bytes, milliseconds, codec)."""
    await drain(ws)
    start = time.monotonic()
    while time.monotonic() - start < 5:
        await ws.send(json.dumps({"type": "request-keyframe"}))
        await ws.send(json.dumps({"type": "request-frame"}))
        try:
            message = await asyncio.wait_for(ws.recv(), 0.5)
        except asyncio.TimeoutError:
            continue
        if not isinstance(message, bytes):
            continue
        frame = parse_frame(message)
        if frame and frame["keyframe"]:
            return frame["bytes"], (time.monotonic() - start) * 1000, frame["codec"]
    return 0, 0.0, 0


async def sample(ws, mode: str, seconds: float) -> dict:
    fps, interval = PROFILES[mode]
    await ws.send(json.dumps({"type": "quality", "mode": mode, "fps": fps}))
    await asyncio.sleep(0.3)
    key_bytes, key_ms, key_codec = await keyframe_size(ws)

    start = time.monotonic()
    end = start + seconds
    frames: list[dict] = []
    arrivals: list[float] = []
    pending = False
    sent_at = 0.0
    latencies: list[float] = []
    last_request = 0.0

    async def receiver() -> None:
        nonlocal pending
        while True:
            message = await ws.recv()
            now = time.monotonic()
            if not isinstance(message, bytes):
                continue
            frame = parse_frame(message)
            if frame is None:
                continue
            frames.append(frame)
            arrivals.append(now)
            if pending:
                latencies.append((now - sent_at) * 1000)
                pending = False

    task = asyncio.create_task(receiver())
    while time.monotonic() < end:
        now = time.monotonic()
        if not pending and now - last_request >= interval:
            pending = True
            sent_at = now
            last_request = now
            await ws.send(json.dumps({"type": "request-frame"}))
        await asyncio.sleep(0.005)
    task.cancel()
    try:
        await task
    except asyncio.CancelledError:
        pass

    elapsed = time.monotonic() - start
    gaps = [(b - a) * 1000 for a, b in zip(arrivals, arrivals[1:])]

    def p95(values: list[float]) -> float:
        if not values:
            return 0.0
        ordered = sorted(values)
        return ordered[min(len(ordered) - 1, int(len(ordered) * 0.95))]

    total = sum(frame["bytes"] for frame in frames)
    return {
        "mode": mode,
        "fps": fps,
        "key_bytes": key_bytes,
        "key_ms": key_ms,
        "key_codec": key_codec,
        "wire_fps": len(frames) / elapsed,
        "painted_fps": sum(1 for f in frames if f["tiles"] > 0) / elapsed,
        "kb_s": total / elapsed / 1024,
        "p95_gap": p95(gaps),
        "rtt_p95": p95(latencies),
    }


async def run(args: argparse.Namespace) -> int:
    base = args.base.rstrip("/")
    parsed = urllib.parse.urlsplit(base)
    token = args.token or os.environ.get("MOLINK_TOKEN", "")
    if not token:
        code = args.code or os.environ.get("MOLINK_CODE", "")
        if not code:
            sys.exit("set MOLINK_CODE (or pass --code, or --token)")
        status, login = http_json(base, "/api/login", code, "POST")
        token = login.get("session_token") or ""
        if status != 200 or not token:
            sys.exit(f"login failed: HTTP {status}; pass --token from "
                     f"`molink-core --print-ready`, or enable the web client")

    ws_scheme = "wss" if parsed.scheme == "https" else "ws"
    ws_url = f"{ws_scheme}://{parsed.netloc}/ws/session"
    async with websockets.connect(ws_url, max_size=64 * 1024 * 1024) as ws:
        await ws.send(json.dumps({"type": "hello", "token": token,
                                  "client": "bench-quality", "protocol": 1}))
        while True:
            message = await asyncio.wait_for(ws.recv(), 5)
            if isinstance(message, str):
                if json.loads(message).get("type") == "welcome":
                    break

        results = [await sample(ws, mode, args.seconds)
                   for mode in ("smooth", "balanced", "quality")]

    print(f"{'mode':9} {'wire':>6} {'painted':>8} {'KB/s':>8} "
          f"{'key':>8} {'key ms':>7} {'codec':>6} {'p95 gap':>8}")
    for r in results:
        print(f"{r['mode']:9} {r['wire_fps']:6.1f} {r['painted_fps']:8.1f} "
              f"{r['kb_s']:8.0f} {r['key_bytes'] / 1024:7.0f}K "
              f"{r['key_ms']:7.0f} {r['key_codec']:6} {r['p95_gap']:7.0f}ms")

    smooth, balanced, quality = results
    checks = [
        ("smooth bandwidth <= 70% balanced",
         smooth["kb_s"] <= balanced["kb_s"] * 0.70,
         f"{smooth['kb_s']:.0f} vs {balanced['kb_s']:.0f} KB/s"),
        ("smooth keyframe <= 70% balanced",
         balanced["key_bytes"] > 0 and
         smooth["key_bytes"] <= balanced["key_bytes"] * 0.70,
         f"{smooth['key_bytes'] / 1024:.0f}K vs "
         f"{balanced['key_bytes'] / 1024:.0f}K"),
        ("smooth wire fps >= 25", smooth["wire_fps"] >= 25,
         f"{smooth['wire_fps']:.1f}"),
        ("balanced wire fps >= 17", balanced["wire_fps"] >= 17,
         f"{balanced['wire_fps']:.1f}"),
        ("quality wire fps >= 13", quality["wire_fps"] >= 13,
         f"{quality['wire_fps']:.1f}"),
        ("smooth p95 gap <= 100 ms", smooth["p95_gap"] <= 100,
         f"{smooth['p95_gap']:.0f} ms"),
        ("balanced p95 gap <= 150 ms", balanced["p95_gap"] <= 150,
         f"{balanced['p95_gap']:.0f} ms"),
        ("quality p95 gap <= 200 ms", quality["p95_gap"] <= 200,
         f"{quality['p95_gap']:.0f} ms"),
    ]
    failed = False
    for label, ok, detail in checks:
        print(("PASS " if ok else "FAIL ") + label + f": {detail}")
        failed = failed or not ok
    return 1 if failed else 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("base", help="e.g. http://host:28418")
    parser.add_argument("--code", help="verification code; prefer MOLINK_CODE")
    parser.add_argument("--token", help="session token; skips /api/login")
    parser.add_argument("--seconds", type=float, default=6.0)
    sys.exit(asyncio.run(run(parser.parse_args())))
