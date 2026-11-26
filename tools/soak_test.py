#!/usr/bin/env python3
"""Stability soak for a running moLink host.

Connects, works, disconnects, and repeats, reporting every failure with the
phase that produced it. It is deliberately black-box: it speaks the same
websocket protocol a page does, so a crash, a hang or a refusal shows up
exactly as a user would see it.

    python3 tools/soak_test.py --host 192.168.2.20 --port 28418 \
        --code 123456 --cycles 20 --intent terminal

The first cycle spends the one-time code; every later cycle reconnects with the
resume token the host handed back, which is what a real client does after a
network drop.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import struct
import sys
import time
from dataclasses import dataclass, field

try:
    import websockets
except ImportError:  # pragma: no cover - operator hint
    sys.exit("websockets is required: pip install websockets")

TERM_OUTPUT = 0x01
TERM_INPUT = 0x02


@dataclass
class CycleResult:
    index: int
    intent: str
    ok: bool
    connect_ms: float = 0.0
    welcome_ms: float = 0.0
    work_ms: float = 0.0
    error: str = ""
    notes: list[str] = field(default_factory=list)


@dataclass
class Soak:
    host: str
    port: int
    token: str
    intent: str
    resume_token: str = ""

    def url(self) -> str:
        token = self.resume_token or self.token
        return f"ws://{self.host}:{self.port}/ws/session?token={token}"

    async def one_cycle(self, index: int) -> CycleResult:
        result = CycleResult(index=index, intent=self.intent, ok=False)
        started = time.monotonic()
        try:
            async with websockets.connect(
                self.url(), max_size=None, open_timeout=10, close_timeout=3
            ) as ws:
                result.connect_ms = (time.monotonic() - started) * 1000
                hello = {
                    "type": "hello",
                    "name": "soak",
                    "device_id": "soak",
                    "intent": self.intent,
                    "clipboard_pull": True,
                    "video_push": self.intent == "desktop",
                    "h264_stream": False,
                    "vp9_stream": False,
                    "vp8_stream": False,
                }
                await ws.send(json.dumps(hello))
                welcome = await self._await_welcome(ws, result)
                if welcome is None:
                    result.error = result.error or "no welcome"
                    return result
                if welcome.get("session_token"):
                    self.resume_token = welcome["session_token"]
                result.welcome_ms = (time.monotonic() - started) * 1000

                work_started = time.monotonic()
                if self.intent == "terminal":
                    await self._terminal_work(ws, welcome, result)
                elif self.intent == "desktop":
                    await self._desktop_work(ws, result)
                result.work_ms = (time.monotonic() - work_started) * 1000
                result.ok = not result.error
        except asyncio.TimeoutError as exc:
            result.error = f"timeout: {exc}"
        except Exception as exc:  # noqa: BLE001 - the point is to record it
            result.error = f"{type(exc).__name__}: {exc}"
        return result

    async def _await_welcome(self, ws, result: CycleResult) -> dict | None:
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            try:
                message = await asyncio.wait_for(ws.recv(), timeout=4)
            except asyncio.TimeoutError:
                result.error = "welcome timeout"
                return None
            if isinstance(message, (bytes, bytearray)):
                continue
            data = json.loads(message)
            if data.get("type") == "welcome":
                return data
            if data.get("type") == "error":
                result.error = f"host error {data.get('code')}: {data.get('message')}"
                return None
        result.error = result.error or "welcome timeout"
        return None

    async def _terminal_work(self, ws, welcome: dict, result: CycleResult) -> None:
        """Open a shell through the session's terminal endpoint and run a command."""
        token = self.resume_token or self.token
        url = f"ws://{self.host}:{self.port}/ws/terminal?token={token}"
        marker = f"soak{int(time.time() * 1000)}"
        try:
            async with websockets.connect(url, max_size=None, open_timeout=8) as term:
                await term.send(json.dumps({"type": "term-open", "cols": 80, "rows": 24}))
                term_id = None
                deadline = time.monotonic() + 8
                while term_id is None and time.monotonic() < deadline:
                    message = await asyncio.wait_for(term.recv(), timeout=4)
                    if isinstance(message, str):
                        data = json.loads(message)
                        if data.get("type") == "term-opened":
                            term_id = data["id"]
                        elif data.get("type") == "error":
                            result.error = f"terminal error {data.get('code')}"
                            return
                if term_id is None:
                    result.error = "terminal open timeout"
                    return
                payload = bytes([TERM_INPUT]) + struct.pack("<Q", term_id)
                await term.send(payload + f"echo {marker}\r".encode())
                seen = b""
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline:
                    message = await asyncio.wait_for(term.recv(), timeout=3)
                    if isinstance(message, (bytes, bytearray)) and message[0] == TERM_OUTPUT:
                        seen += bytes(message[1:])
                        if marker.encode() in seen and seen.count(marker.encode()) >= 2:
                            break
                if seen.count(marker.encode()) < 2:
                    result.error = "terminal output missing"
                    result.notes.append(seen[-120:].decode("utf-8", "replace"))
                await term.send(json.dumps({"type": "term-close", "id": term_id}))
        except Exception as exc:  # noqa: BLE001
            result.error = f"terminal {type(exc).__name__}: {exc}"

    async def _desktop_work(self, ws, result: CycleResult) -> None:
        """Ask for a keyframe and confirm at least one frame arrives."""
        await ws.send(json.dumps({"type": "keyframe"}))
        deadline = time.monotonic() + 8
        frames = 0
        while time.monotonic() < deadline:
            try:
                message = await asyncio.wait_for(ws.recv(), timeout=2)
            except asyncio.TimeoutError:
                break
            if isinstance(message, (bytes, bytearray)):
                frames += 1
                if frames >= 1:
                    return
            else:
                data = json.loads(message)
                if data.get("type") == "error":
                    result.error = f"host error {data.get('code')}"
                    return
        result.error = "no video frame"

    async def run(self, cycles: int, pause: float) -> int:
        results = []
        for index in range(1, cycles + 1):
            result = await self.one_cycle(index)
            results.append(result)
            status = "ok" if result.ok else f"FAIL ({result.error})"
            print(
                f"[{index:>3}/{cycles}] {result.intent:<8} {status:<48}"
                f" connect={result.connect_ms:6.0f}ms welcome={result.welcome_ms:6.0f}ms"
                f" work={result.work_ms:6.0f}ms",
                flush=True,
            )
            for note in result.notes:
                print(f"        note: {note}", flush=True)
            await asyncio.sleep(pause)
        failed = [r for r in results if not r.ok]
        print()
        print(f"cycles={len(results)} ok={len(results) - len(failed)} failed={len(failed)}")
        if failed:
            kinds: dict[str, int] = {}
            for item in failed:
                kinds[item.error] = kinds.get(item.error, 0) + 1
            for error, count in sorted(kinds.items(), key=lambda kv: -kv[1]):
                print(f"  {count:>3}x {error}")
        times = sorted(r.welcome_ms for r in results if r.ok)
        if times:
            p50 = times[len(times) // 2]
            p95 = times[min(len(times) - 1, int(len(times) * 0.95))]
            print(f"welcome latency p50={p50:.0f}ms p95={p95:.0f}ms")
        return 1 if failed else 0


def main() -> int:
    parser = argparse.ArgumentParser(description="moLink stability soak")
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=28418)
    parser.add_argument("--code", required=True, help="one-time code or resume token")
    parser.add_argument("--cycles", type=int, default=10)
    parser.add_argument(
        "--intent",
        default="terminal",
        choices=["desktop", "terminal", "files", "chat"],
    )
    parser.add_argument("--pause", type=float, default=0.5)
    args = parser.parse_args()
    soak = Soak(args.host, args.port, args.code, args.intent)
    return asyncio.run(soak.run(args.cycles, args.pause))


if __name__ == "__main__":
    raise SystemExit(main())
