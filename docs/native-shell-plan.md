# Native shell integration plan

The Direct2D prototype (`shell/windows/ui_kit_demo.cpp` + `ui_kit.h/.cpp`) is
the visual and interaction reference. This is how it becomes the real shell.

## Target states
| build | UI | when |
|---|---|---|
| `moLink.exe` (x64) | WebView2 (React page) | kept, x64 only |
| `moLink-native.exe` (x64 + Win7 x86) | Direct2D + `ui_kit` | new, high-performance client |

## Step 1 — real session data
- The shell starts `molink-core` (as today) and talks to it over the loopback
  WS (`/ws/session` for a direct host, or the core's proxy endpoints).
- Feed the toolbar from the session: `stat_codec`, `stat_kbps`,
  `stat_latency_ms`, `stat_resolution` (already fields in the demo).
- Wire the quality panel to the `quality` message (`mode`, `qp`,
  `bitrate_kbps`, `rate_mode`, `format`, `fps`) that the web UI already uses.

## Step 2 — picture
- The core publishes composed tiles to the frame surface (stage 1, done);
  the shell opens it with `--surface` (already prototyped) and draws it in the
  stage rect. Streams (VP9/AV1/H.264) need core-side decode first.

## Step 3 — interactions
- Home: connect (address + code fields already editable) → open a session tab.
- Tabs: one tab per session; overflow folds into a "更多" menu.
- Settings: 外观 / 连接 / 传输 cards (already laid out).
- Avatar: upload, rounded clip, persisted path (done).
- Toast on connect success/failure; tooltip on icon-only buttons (kit ready).

## Step 4 — packaging
- x64: MinGW64 MSVCRT toolchain (`tools/mingw64-msvcrt`), replace the MSVC
  Windows job once parity is reached.
- Win7 x86: mingw32 + libuvcpp 1.8.5 kit; GDI/D2D1 (no WebView2).
- Browser shell keeps WebView2 and ships x64 only.

## Open items
- Tabs wider than the strip → fold into a menu.
- DPI 150 %/200 % pass; listen for `WM_SETTINGCHANGE` to follow the system theme.
- Core-side VP9/AV1 decode so the native picture works for streams, not just
  tiles.

## WS contract for the shell's client (step 1 detail)

The shell connects to its own core (the one it started) — not to the remote
host — and lets the core do transport selection:

- `ws://127.0.0.1:<port>/ws/direct?host=<host>&port=<port>` for a typed
  address, or `/ws/connect?target=<device-id>` for an ID-server session.
- Send `hello {token: <client_token from the ready line>, seq_clock: true,
  video_push: true, h264_stream, vp9_stream, vp8_stream, av1_native}`.
- Handle: `welcome` (stream_codecs, tiles.screen_w/h, codec_preference),
  `codec` (wire, wire_rate, full), `ping`/`pong` (RTT), `quality` (ack of a
  preset/slider change), `transport` (path/protocol for the status chip),
  `error`.
- Send: `quality {mode, fps, qp, bitrate_kbps, rate_mode, format}`,
  `stream-codec {value}`, `viewer {fps, dropped, rx, rtt}` once a second,
  `request-keyframe`.

Binary frames are the picture; the native shell ignores them (the shared
surface carries the composed picture) except for the seq clock.

Implementation options, cheapest first:
1. **Minimal WS client over Winsock** (~400 lines): HTTP Upgrade, masked
   client frames, ping/pong, close, reconnect. No dependencies; works on Win7.
2. **libuvcpp's web client** (already shipped as uvcpp.dll): less code, but
   the shell would depend on the uvcpp DLL and its loop. Prefer this if the
   API fits.
