#!/usr/bin/env python3
"""Desktop-intent stress that kills the core: connect, take a couple of frames,
then drop the TCP connection without a close frame. Repeats until the host
refuses connections (i.e. it died) or the cycle budget is spent."""

import argparse
import asyncio
import json
import sys
import urllib.request

import websockets


def fetch_code(port: int) -> str:
    with urllib.request.urlopen(f"http://127.0.0.1:{port}/api/device", timeout=3) as reply:
        return json.loads(reply.read())["code"]["value"]


async def cycle(port: int, token: str) -> str:
    async with websockets.connect(
        f"ws://127.0.0.1:{port}/ws/session?token={token}", max_size=None, open_timeout=5
    ) as ws:
        await ws.send(
            json.dumps(
                {
                    "type": "hello",
                    "name": "stress",
                    "intent": "desktop",
                    "video_push": True,
                    "h264_stream": True,
                    "vp9_stream": True,
                    "vp8_stream": True,
                    "clipboard_pull": True,
                }
            )
        )
        frames = 0
        for _ in range(30):
            try:
                message = await asyncio.wait_for(ws.recv(), 1.0)
            except asyncio.TimeoutError:
                break
            if isinstance(message, (bytes, bytearray)):
                frames += 1
                if frames >= 2:
                    break
            else:
                data = json.loads(message)
                if data.get("type") == "error":
                    return data.get("code", "error")
        ws.transport.abort()  # abrupt: no close frame, just RST
    return "ok"


async def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=28561)
    parser.add_argument("--cycles", type=int, default=300)
    args = parser.parse_args()
    token = fetch_code(args.port)
    refused = 0
    results: dict[str, int] = {}
    for index in range(1, args.cycles + 1):
        try:
            outcome = await cycle(args.port, token)
        except Exception as exc:  # noqa: BLE001 - any failure is data
            outcome = f"exc {type(exc).__name__}"
            if isinstance(exc, (ConnectionRefusedError, OSError)):
                refused += 1
        results[outcome] = results.get(outcome, 0) + 1
        if refused > 3:
            print(f"core 挂了：第 {index} 次循环开始连不上", flush=True)
            break
        await asyncio.sleep(0.03)
    print(f"cycles={index} outcomes={results}")
    return 1 if refused > 3 else 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
