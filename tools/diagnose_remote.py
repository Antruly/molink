#!/usr/bin/env python3
"""Measure what a remote moLink core is actually sending.

Run this from a machine that can reach the core (the same network as the
browser you use for the remote session):

    MOLINK_CODE='the-verification-code' \
      python3 tools/diagnose_remote.py http://host:28418

The code is read from MOLINK_CODE so it does not land in shell history. The
script logs in, reads /api/info, then samples the WebSocket: codec, frame and
tile sizes, bytes/s, RTT and request-to-frame latency. It repeats the sample at
8 fps and 5 fps to show whether lowering the capture rate helps.
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


def http_json(base: str, path: str, token: str | None = None,
              method: str = "GET") -> tuple[int, dict]:
    request = urllib.request.Request(base + path, method=method)
    if token:
        request.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(request, timeout=15) as response:
            body = response.read()
            return response.status, json.loads(body or b"{}")
    except urllib.error.HTTPError as error:
        body = error.read()
        try:
            parsed = json.loads(body or b"{}")
        except Exception:
            parsed = {"raw": body.decode("utf-8", "replace")}
        return error.code, parsed


def packet_frame(data: bytes) -> dict | None:
    if len(data) < 32 or data[:4] != b"MOLK":
        return None
    payload_len = struct.unpack_from("<I", data, 24)[0]
    payload = data[32:32 + payload_len]
    if len(payload) < 16:
        return None
    frame_seq, tile_count, codec, flags, w, h, tw, th = struct.unpack_from(
        "<IHBBHHHH", payload, 0
    )
    return {
        "bytes": len(data),
        "payload": len(payload),
        "seq": frame_seq,
        "tiles": tile_count,
        "codec": codec,
        "flags": flags,
        "screen": (w, h),
        "tile": (tw, th),
    }


def summarize(label: str, samples: list[dict], rtts: list[float],
              latencies: list[float], seconds: float) -> None:
    total = sum(s["bytes"] for s in samples)
    codecs: dict[int, int] = {}
    for sample in samples:
        codecs[sample["codec"]] = codecs.get(sample["codec"], 0) + 1

    def pct(values: list[float], p: float) -> float:
        if not values:
            return 0.0
        ordered = sorted(values)
        return ordered[min(len(ordered) - 1, int(len(ordered) * p))]

    print(f"\n== {label} ({seconds:.1f}s) ==")
    print(f"frames: {len(samples)}  fps: {len(samples) / seconds:.1f}")
    print(f"bytes: {total}  throughput: {total / seconds / 1024 / 1024:.2f} MiB/s")
    if samples:
        sizes = [s["bytes"] for s in samples]
        print(f"frame bytes: avg={statistics.mean(sizes):.0f} "
              f"p95={pct(sizes, 0.95):.0f} max={max(sizes)}")
        print(f"tiles: avg={statistics.mean([s['tiles'] for s in samples]):.1f} "
              f"max={max(s['tiles'] for s in samples)}")
        print(f"codecs: {codecs}  (1=BGRA raw, 5=zlib deflate)")
        print(f"screen: {samples[-1]['screen']}  tile: {samples[-1]['tile']}")
        keyframes = sum(1 for s in samples if s["flags"] & 1)
        print(f"keyframes: {keyframes}")
    if rtts:
        print(f"RTT ms: avg={statistics.mean(rtts):.0f} "
              f"max={max(rtts):.0f}")
    if latencies:
        print(f"request->frame ms: avg={statistics.mean(latencies):.0f} "
              f"p95={pct(latencies, 0.95):.0f} max={max(latencies):.0f}")


async def sample(ws, seconds: float, fps: int | None,
                 label: str) -> None:
    if fps is not None:
        await ws.send(json.dumps({"type": "capture-fps", "fps": fps}))
        await asyncio.sleep(0.3)

    samples: list[dict] = []
    rtts: list[float] = []
    latencies: list[float] = []
    pending = False
    sent_at = 0.0
    start = time.monotonic()
    last_request = 0.0
    last_ping = 0.0

    async def receiver():
        nonlocal pending
        while True:
            message = await ws.recv()
            now = time.monotonic()
            if isinstance(message, str):
                payload = json.loads(message)
                if payload.get("type") == "pong" and "t" in payload:
                    rtts.append((now * 1000) - float(payload["t"]))
                continue
            frame = packet_frame(message)
            if frame is None:
                continue
            samples.append(frame)
            if pending:
                latencies.append((now - sent_at) * 1000)
                pending = False

    receiver_task = asyncio.create_task(receiver())
    while time.monotonic() - start < seconds:
        now = time.monotonic()
        if now - last_request >= 0.06 and not pending:
            pending = True
            sent_at = now
            last_request = now
            await ws.send(json.dumps({"type": "request-frame"}))
        if now - last_ping >= 1.0:
            last_ping = now
            await ws.send(json.dumps({"type": "ping", "t": now * 1000}))
        await asyncio.sleep(0.01)
    receiver_task.cancel()
    try:
        await receiver_task
    except asyncio.CancelledError:
        pass
    summarize(label, samples, rtts, latencies, seconds)


async def run(args: argparse.Namespace) -> None:
    base = args.base.rstrip("/")
    parsed = urllib.parse.urlsplit(base)
    status, health = http_json(base, "/healthz")
    print(f"health: HTTP {status} {health}")
    status, state = http_json(base, "/api/state")
    print(f"state: HTTP {status} {json.dumps(state, ensure_ascii=False)}")

    code = args.code or os.environ.get("MOLINK_CODE", "")
    if not code:
        sys.exit("set MOLINK_CODE (or pass --code) to the verification code")

    status, login = http_json(base, "/api/login", code, "POST")
    print(f"login: HTTP {status} "
          f"{ {k: ('<redacted>' if 'token' in k else v) for k, v in login.items()} }")
    token = login.get("session_token") or code
    if status != 200:
        print("login failed; if this is a remote browser, allow_web_client must be on",
              file=sys.stderr)
        return

    status, info = http_json(base, "/api/info", token)
    print(f"info: HTTP {status} {json.dumps(info, ensure_ascii=False)}")

    ws_scheme = "wss" if parsed.scheme == "https" else "ws"
    ws_url = f"{ws_scheme}://{parsed.netloc}/ws/session"
    async with websockets.connect(ws_url, max_size=64 * 1024 * 1024) as ws:
        await ws.send(json.dumps({"type": "hello", "token": token,
                                  "client": "diagnose", "protocol": 1}))
        while True:
            message = await asyncio.wait_for(ws.recv(), 5)
            if isinstance(message, str):
                payload = json.loads(message)
                if payload.get("type") == "welcome":
                    print("welcome:", json.dumps(payload, ensure_ascii=False))
                    break

        await sample(ws, args.seconds, None, f"current ({args.seconds:.0f}s)")
        await sample(ws, args.seconds, 8, f"capture-fps=8 ({args.seconds:.0f}s)")
        await sample(ws, args.seconds, 5, f"capture-fps=5 ({args.seconds:.0f}s)")
        await asyncio.sleep(0.2)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("base", help="e.g. http://host:28418")
    parser.add_argument("--code", help="verification code; prefer MOLINK_CODE")
    parser.add_argument("--seconds", type=float, default=6.0)
    asyncio.run(run(parser.parse_args()))
