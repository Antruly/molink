#!/usr/bin/env python3
"""End-to-end check of the host-driven video clock (`video_push`).

Starts molink-core with synthetic frames on a loopback port, then opens two
sessions over a real WebSocket:

  * a client that advertises `video_push` in hello must receive frames
    without ever sending `request-frame`;
  * a client that does not must receive nothing until it pulls, and must
    still get frames when it does -- the compatibility path.

Usage:
  python3 tools/smoke_video_push.py [--core build/core/molink-core]
                                    [--capture-fps 30] [--seconds 3]

Requires the `websockets` package. Exit code 0 means both sessions behaved.
"""

import argparse
import asyncio
import json
import os
import subprocess
import sys
import time

try:
    import websockets
except ImportError:  # pragma: no cover - environment, not logic
    sys.exit("this check needs the `websockets` package (pip install websockets)")

PACKET_MAGIC = b"MOLK"
PACKET_TYPE_FRAME = 0x02
PACKET_HEADER_BYTES = 32


def is_frame(data: bytes) -> bool:
    return (
        len(data) >= PACKET_HEADER_BYTES
        and data[:4] == PACKET_MAGIC
        and data[5] == PACKET_TYPE_FRAME
    )


def start_core(core: str, capture_fps: int) -> tuple[subprocess.Popen, int, str]:
    proc = subprocess.Popen(
        [
            core,
            "--host", "127.0.0.1",
            "--port", "0",
            "--no-capture",
            "--no-input",
            "--no-clipboard",
            "--capture-fps", str(capture_fps),
            "--print-ready",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    deadline = time.time() + 20
    while time.time() < deadline:
        line = proc.stdout.readline()
        if not line:
            break
        if '"event":"ready"' in line:
            ready = json.loads(line)
            return proc, int(ready["port"]), ready["token"]
    proc.kill()
    raise RuntimeError("molink-core did not report ready")


async def session(url: str, token: str, ask_push: bool, seconds: float) -> dict:
    """Hello, then sit idle and count what arrives without asking for it."""
    async with websockets.connect(url, max_size=None) as ws:
        hello = {
            "type": "hello",
            "token": token,
            "protocol": 1,
            "client": "smoke",
            "seq_clock": True,
        }
        if ask_push:
            hello["video_push"] = True
        await ws.send(json.dumps(hello))

        welcome = None
        unrequested = 0
        end = asyncio.get_running_loop().time() + seconds
        while asyncio.get_running_loop().time() < end:
            try:
                message = await asyncio.wait_for(ws.recv(), timeout=0.25)
            except asyncio.TimeoutError:
                continue
            if isinstance(message, str):
                document = json.loads(message)
                if document.get("type") == "welcome":
                    welcome = document
            elif is_frame(message):
                unrequested += 1

        # A pull must still work on either kind of session.
        pulled = 0
        end = asyncio.get_running_loop().time() + 2.0
        while asyncio.get_running_loop().time() < end:
            await ws.send(json.dumps({"type": "request-frame"}))
            try:
                message = await asyncio.wait_for(ws.recv(), timeout=0.5)
            except asyncio.TimeoutError:
                continue
            if isinstance(message, bytes) and is_frame(message):
                pulled += 1
            await asyncio.sleep(1 / 30)

        return {
            "advertised_push": bool(welcome and welcome.get("video_push")),
            "unrequested": unrequested,
            "pulled": pulled,
        }


async def count_frames(ws, seconds: float) -> int:
    frames = 0
    end = asyncio.get_running_loop().time() + seconds
    while asyncio.get_running_loop().time() < end:
        try:
            message = await asyncio.wait_for(ws.recv(), timeout=0.25)
        except asyncio.TimeoutError:
            continue
        if isinstance(message, bytes) and is_frame(message):
            frames += 1
    return frames


async def toggle(url: str, token: str) -> dict:
    """A hidden page must be able to hand the session back to its own pull."""
    async with websockets.connect(url, max_size=None) as ws:
        await ws.send(json.dumps({
            "type": "hello", "token": token, "protocol": 1,
            "client": "smoke", "video_push": True,
        }))
        # Skip the welcome, then measure the clock.
        await ws.recv()
        before = await count_frames(ws, 1.5)
        await ws.send(json.dumps({"type": "video-push", "on": False}))
        # Frames already in flight need a moment to clear.
        await count_frames(ws, 0.5)
        off = await count_frames(ws, 1.5)
        await ws.send(json.dumps({"type": "video-push", "on": True}))
        back_on = await count_frames(ws, 1.5)
        return {"before": before, "off": off, "back_on": back_on}


async def run(core: str, capture_fps: int, seconds: float) -> int:
    proc, port, token = start_core(core, capture_fps)
    try:
        url = f"ws://127.0.0.1:{port}/ws/session"
        pushed = await session(url, token, ask_push=True, seconds=seconds)
        pulled_only = await session(url, token, ask_push=False, seconds=seconds)
        toggled = await toggle(url, token)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    print(f"push-capable : {pushed}")
    print(f"pull-only    : {pulled_only}")
    print(f"hidden toggle: {toggled}")

    failures = []
    if not pushed["advertised_push"]:
        failures.append("the host did not advertise video_push in welcome")
    if pushed["unrequested"] <= 0:
        failures.append("a push-capable session received no frames on its own")
    if pulled_only["unrequested"] > 0:
        failures.append("a pull-only session received frames it never asked for")
    if pushed["pulled"] <= 0 or pulled_only["pulled"] <= 0:
        failures.append("request-frame stopped working")
    if toggled["before"] <= 0:
        failures.append("the clock was not running before the toggle")
    if toggled["off"] > toggled["before"] / 4:
        failures.append("turning the clock off did not stop pushed frames")
    if toggled["back_on"] <= 0:
        failures.append("turning the clock back on did not resume frames")

    for failure in failures:
        print(f"FAIL: {failure}")
    print("SMOKE", "PASS" if not failures else "FAIL")
    return 0 if not failures else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--core",
        default=os.path.join(os.path.dirname(__file__), "..", "build", "core",
                             "molink-core"),
    )
    parser.add_argument("--capture-fps", type=int, default=30)
    parser.add_argument("--seconds", type=float, default=3.0)
    args = parser.parse_args()
    return asyncio.run(run(args.core, args.capture_fps, args.seconds))


if __name__ == "__main__":
    sys.exit(main())
