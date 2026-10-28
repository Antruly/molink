#!/usr/bin/env python3
"""Checks the host's side of the WebRTC handshake against a real core.

Starts molink-core (a build with MOLINK_ENABLE_WEBRTC=ON), opens one session
that advertises `webrtc` in hello, and expects:

  * `welcome.webrtc` to be true,
  * an `rtc-offer` control message with an SDP that has a video section and a
    data channel,

which is everything the browser needs to answer. The answer itself needs a
real RTCPeerConnection, so it is not exercised here (the C++ loopback test
covers the full exchange).

Usage:
  python3 tools/smoke_webrtc_offer.py [--core build/webrtc/molink-core]
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
    sys.exit("this check needs the `websockets` package")


def start_core(core: str) -> tuple[subprocess.Popen, int, str]:
    proc = subprocess.Popen(
        [core, "--host", "127.0.0.1", "--port", "0", "--no-capture",
         "--no-input", "--no-clipboard", "--print-ready"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
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


async def run(core: str) -> int:
    proc, port, token = start_core(core)
    failures = []
    try:
        url = f"ws://127.0.0.1:{port}/ws/session"
        async with websockets.connect(url, max_size=None) as ws:
            await ws.send(json.dumps({
                "type": "hello", "token": token, "protocol": 1,
                "client": "smoke", "webrtc": True,
            }))
            welcome = None
            offer = None
            end = asyncio.get_running_loop().time() + 10
            while asyncio.get_running_loop().time() < end and offer is None:
                try:
                    message = await asyncio.wait_for(ws.recv(), timeout=0.5)
                except asyncio.TimeoutError:
                    continue
                if not isinstance(message, str):
                    continue
                document = json.loads(message)
                if document.get("type") == "welcome":
                    welcome = document
                elif document.get("type") == "rtc-offer":
                    offer = document

            if welcome is None:
                failures.append("no welcome")
            elif welcome.get("webrtc") is not True:
                failures.append("welcome does not advertise webrtc")
            if offer is None:
                failures.append("no rtc-offer was sent")
            else:
                sdp = offer.get("sdp", "")
                if "m=video" not in sdp:
                    failures.append("the offer has no video section")
                if "m=application" not in sdp:
                    failures.append("the offer has no data channel section")
                if "H264" not in sdp:
                    failures.append("the offer does not name H.264")
                print(f"offer: {len(sdp)} bytes, "
                      f"video={'m=video' in sdp}, "
                      f"data={'m=application' in sdp}, "
                      f"h264={'H264' in sdp}")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    for failure in failures:
        print(f"FAIL: {failure}")
    print("SMOKE", "PASS" if not failures else "FAIL")
    return 0 if not failures else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--core",
        default=os.path.join(os.path.dirname(__file__), "..", "build",
                             "webrtc", "molink-core"),
    )
    args = parser.parse_args()
    return asyncio.run(run(args.core))


if __name__ == "__main__":
    sys.exit(main())
