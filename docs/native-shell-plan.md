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

## Home (rebuilt 2026-10-31)

The demo's home now follows the web's `SimpleHomeView`:

- **SelfCard**: 58 px avatar with a pencil badge, editable name, platform and
  ID-server chips, 20 px device ID with copy, IP, the verification-code box
  (eye/copy/refresh, single/multi pills, remaining uses) and the 更多设置 link.
- **ConnectCard**: 远程桌面/文件传输/聊天 segmented control, the large target
  input with an inline send button, the optional code field with an eye
  toggle, 强制中继 checkbox, the primary 连接/发送连接请求 button and the port
  hint.
- **NoticePanel**: ID-server status, update card (发现新版本 + 立即更新) and
  the NAT hint card.
- **DeviceList**: 最近/已保存/局域网/联网 tabs, cards/list toggles and the
  per-tab dashed empty states. Successful sessions are remembered (name,
  target, platform, online, last seen, and the access code when one was used)
  in `%APPDATA%\moLink\ui-recents.txt` and shown as cards (or rows) with
  per-device 远程桌面/文件传输/聊天 quick actions. A row with a saved code
  connects with it; clicking a card prefills the form (address + code) and
  focuses the code field like the web's `prefillFromRecent`, shows a 已保存
  chip, and the trash action forgets the row. The token is an optional sixth
  column, so five-column files from older builds still load.
- **Close prompt**: the window X (or Alt+F4) with a session open asks
  取消 / 关闭标签页 / 最小化到托盘, with 记住我的选择; the choice persists in
  `HKCU\Software\moLink\Ui\WindowCloseBehavior`. With no session the window
  closes. 最小化到托盘 really hides the window and adds a notification-area
  icon (double-click restores; right-click offers 显示 / 退出); if the shell
  has no tray the window falls back to a taskbar minimize. A minimized window
  no longer resizes the D2D swap chain (that left a stale back buffer under
  Wine).
- **Tab close**: the session tab's X removes the tab and stops the session.
  It also bumps a connect generation, so an in-flight connect or reconnect
  worker cannot re-open the tab the user just closed. The header 首页 tab now
  uses the web's `px-3` / 14 px icon / `gap-1.5` padding.
- **StatusBar**: ID-server state, connection state and version.
- The whole home scrolls inside the area below the header (wheel + scrollbar),
  exactly like the web's `overflow-y-auto` main.
- **Connecting is real**: the ConnectCard starts a `/ws/session` connection to
  `<host>:<port>` with the entered code on a worker thread, so a slow host
  never freezes the UI. `welcome` feeds the codec/resolution stats and the
  host's `device_name` becomes the session-tab label; the toolbar's quality,
  codec, resolution, audio, clipboard, SAS and chat actions go over that
  socket. A failed attempt shows an error toast, and a dropped connection
  shows 会话已断开. The prototype connects **directly to the host core** (no
  local-core proxy / ID-server lookup / P2P ladder yet).
- **Picture**: after `hello` the client asks for `stream-codec tiles` and
  `format bgra`. Binary frame packets are parsed (MOLK packet header, frame
  header, tile table) and composited into a BGRA surface: bgra/rgb24/rgb565/
  gray8 and their deflate variants (inflated with the vendored `puff.c`).
  PNG/JPEG tiles and H.264/VP9/AV1 streams are not decoded yet. The
  shared-memory surface (`--surface`) still takes priority when present.
- **Local-core proxy (experimental)**: `--proxy` (or `--core <molink-core.exe>`)
  starts the core with `MOLINK_SURFACE=1` and `--print-ready --port 0`, reads
  its ready line (port, `client_token`, surface path) and connects through
  `/ws/direct?host=...&port=...&client=...` instead of dialling the host
  directly. The core then performs the ID-server/P2P ladder and decodes
  H.264/VP9/AV1, publishing the composed picture to the shared-memory
  surface, which the demo draws (surface takes priority over tile frames).
  Mechanisms were validated with a fake `molink-core.exe` and a mock
  `/ws/direct` proxy; a real core + remote host could not be tested from this
  Linux box.
- **Access requests**: leaving the code empty sends
  `{type:"request-access",name:"moLink native",client:"molink-native"}` instead
  of `hello`. The session tab shows a pulsing amber dot and the picture area
  says 已发送连接请求，等待主机确认…; an approved host then sends `welcome` and
  the normal picture/TLS badge appear. Host `error` replies are mapped to the
  web's Chinese strings (invalid_code → 验证码错误或已失效, access_denied →
  对方拒绝了本次连接请求, locked, too_many_attempts, ip_banned, display-mode
  errors) and shown in a persistent red bar over the picture plus a toast; the
  tab dot turns red. A wrong code puts an inline 验证码 field and 连接 button in
  that bar (web `needsCode`), other errors get a 重新连接 button. When a
  transport only delivers the WebSocket close frame (1008 + "invalid token" /
  "locked" / "access denied" / ...), the close reason is classified the same
  way, matching the web client's close-reason fallback. The bar keeps
  clear of the chat/file drawers like the web's flex layout. Enter in either
  home field starts the same flow as the button, and the selected
  远程桌面/文件传输/聊天 intent opens the matching panel once the host
  authenticates.
- **Reconnect**: after an accepted session drops (socket close without a
  policy error), the client retries immediately, then at 1 s / 1 s / every
  10 s while the tab stays open; the last frame stays under a 连接已断开，
  正在重连… chip and the tab dot pulses amber. A `welcome` on the replacement
  socket ends the cycle with 已重新连接. Rejected codes, denied access and
  never-accepted requests do not retry (the red bar is final). The reconnect
  reuses the saved target/code and keeps the visible frame.
- **Text field menu**: right-click in the home address/code, chat composer or
  session retry field opens a 剪切/复制/粘贴/全选 menu (disabled items greyed
  by selection/clipboard state). Double-click selects the whole value,
  Ctrl+A/C/X/V work everywhere, and Esc closes the menu.
- **Chat / clipboard receive**: incoming `chat action=message` text is
  appended to the drawer with an unread dot on the collapsed ball;
  `clipboard`/`clipboard-sync` text is placed on the Windows clipboard when
  clipboard sync is on (the chat composer can paste it).
- **Text fields**: the home address/code and chat composer support click
  caret placement, mouse-drag/Shift selection, Ctrl+A/C/X/V and typing over a
  selection; selection is highlighted in accent.
- **Input**: with the control toggle on, exact browser-style input packets are
  sent over the same socket — `move` with normalized canvas coordinates,
  `button` 1/2/3, `wheel`, and `key` with `KeyboardEvent.code` names
  (KeyA/Digit1/ArrowLeft/ControlLeft/F1/...). Coordinates follow the painted
  image rect for 适应/1:1/拉伸. Toggling control off or closing the session
  releases held buttons.

## Session toolbar (rebuilt 2026-10-31)

The demo's session page now follows the web's `SessionView` toolbar:

- **Ball**: 40x40 at the picture's top centre (centre-18, top 12), drag to move
  (5 px threshold, drag suppresses the click), click opens the bar.
- **Bar**: centred at the top of the picture, surface/95, 38 px tall. The
  widest of three tiers that fits is used: full (every control), compact (the
  web's clipboard/tools/appearance `ToolbarMenu`s) and minimal.
- **Pin** persists in `HKCU\Software\moLink\Ui\ToolbarPinned`; a pinned bar
  survives clicks on the picture, an unpinned one collapses (and Esc closes
  it). Changing the display mode also collapses an unpinned bar, like the web.
- **Quality** opens the web `QualityPanel` as a 316x282 popover: preset grid,
  rate-mode switch, quality/bitrate/fps sliders (the quality slider is RTL),
  image format and reset.
- **FPS / codec / resolution** are native select menus; the image format menu
  lives inside the quality popover.
- **Hover**: every button uses the web's 140 ms transition (panel tint plus an
  accent-mixed border), the tab and window buttons turn accent/danger on
  hover, the avatar shows a ring and a change-picture overlay, and tooltips
  appear after 600 ms. The cursor becomes a hand over controls, a move cursor
  over the ball/bar background and an I-beam in the home form.
- **Watermark** is the web's overlay: diagnostics chips on the top-left and a
  fps/latency/bandwidth/encoding/transport bar on the bottom-right.
- **Settings** opens the web's centered `SettingsPanel`: a 768 px dialog with
  a dimmed backdrop, the web's six sections and its real content:
  - 通用: 亮色/暗色 segment, 自动锁定 dropdown, English/中文 segment.
  - 安全: six switches (LAN/IP, web client, discovery, access requests,
    remote/LAN encryption), STUN server, device name and long-term code.
  - 网络: TCP/UDP port fields with 保存端口, plus the server block
    (启用远程目录, 服务端地址, 注册邀请码, 保存并注册).
  - 显示: watermark and auto-audio switches, average frame-rate dropdown,
    and the eight default-quality cards with descriptions.
  - 服务: 后台服务 status and install/start/stop/restart/uninstall buttons.
  - 关于: product card, repository/website/license rows, star card,
    check-update card and the privacy statement.
  The content pane scrolls with the wheel and shows a scrollbar; backdrop
  click, the X and Esc close the dialog, which fades/scales in over 140 ms.
  The theme mode persists in `HKCU\Software\moLink\Ui\ThemeMode`.
- **Chat** is the web's 340 px right drawer: remote chip, dashed empty state,
  message bubbles and a composer. Sent messages echo locally and go over the
  session as `{type:"chat",action:"send",text}`; incoming message/history
  handling is the next wiring step.
- **Files** is the web's 380 px right drawer with 发送 / 接收 tabs:
  - 发送 has the dashed drop zone (click opens the Windows picker,
    `WM_DROPFILES` accepts drops) and offers each file to the host with the
    web's `{type:"file",action:"offer",name,size,dest}` flow. Chunks are
    `file_data` (0x12) packets: the 32-byte MOLK header, then u32 id, u32
    reserved, u64 offset and the bytes. The host's `accept{id,chunk}` starts
    the upload, `complete{id}` ends it, `done{ok,path}`/`reject`/`error`
    finish the row. Two chunks per 16 ms tick at most, and `complete` waits
    one tick after the final chunk so a control message cannot overtake the
    last binary frame.
  - 接收 browses the host's home folder (`browse` → `listing` with
    name/dir/size/mtime), navigates with the up/refresh buttons and the mouse
    wheel, and downloads a clicked file (`request` → `sending` → pull-driven
    `file_data` chunks → `eof`). Files land in
    `%USERPROFILE%\Downloads\moLink` with a unique name; a truncated or
    out-of-order transfer deletes the partial file and shows 失败. The host
    may deliver `eof` before the last chunk, so the client keeps `eof_seen`
    and finishes only once every byte has arrived (3 s grace, then fails).
  - The bottom list shows up to three transfers with per-file progress, a
    cancel X for active rows, and toasts for sent/saved/failed.
- **Session messages**: codec, resolution, audio start/stop, Ctrl+Alt+Del and
  clipboard get/set now send the web's `stream-codec`, `display-mode`,
  `audio`, `input` and `clipboard` JSON when a session is attached.
- **Click feedback**: every button flashes the accent-soft press state for
  130 ms, on top of the 140 ms hover transition.

## Stream decode in the local core (2026-10-31)

The native picture no longer stops at tiles. `surface_compositor` decodes the
low-delay streams and publishes whole frames to the shared surface:

- **AV1** (`codec::av1_stream`) through the existing dav1d stream decoder
  (`av1_stream_decoder` in `capture/av1_frame.cpp`); deltas wait for a
  keyframe, a failed decode resets the decoder.
- **VP9/VP8** through libvpx (`vpx_codec_dec_init` + I420 → BGRA) when the
  build has `MOLINK_HAVE_VPX`.
- **H.264** still needs a hardware decoder and stays off this path.

A viewer core that has the surface enabled therefore paints streams without
the page. The D2D client gains a **surface stream mode** (`--surface` +
`MOLINK_CORE_TOKEN`/`MOLINK_CORE_PORT` from the core's ready line): hello
advertises `av1_native`/`vp9_stream`/`vp8_stream`, the proxy injects
`av1_stream`, the client asks for `stream-codec: av1`, and tiles/refresh
fallbacks are skipped because the surface wins. Measured on the Win10 host
(CPU SVT-AV1) through a locally built core: 1920×1080 at ~21–30 fps,
9–12 ms RTT, ~635 kbps, codec chip `av1`.

Decoding and publishing run on a dedicated decode thread: the network loop
only enqueues frame packets (512 / 96 MB cap; on overflow the backlog is
dropped and deltas wait for a keyframe), so a 10–30 ms 1080p decode never
stalls other transports. `frame_surface::publish` is serialised because each
viewer proxy has its own compositor. The DXGI capture no longer re-encodes an
unchanged desktop every tick while streaming: it sends a one-second heartbeat
instead, which removes the ~250 KB/s idle cost (needs the rebuilt host core).

Build the core with the decoders (dev trees unpacked with `dpkg-deb -x`):

```
cmake -S backend -B build/live -DMOLINK_ENABLE_AV1=ON \
  -DMOLINK_AV1_SDK=/tmp/av1-sdk -DMOLINK_ENABLE_VPX=ON \
  -DVPX_INCLUDE_DIR=/tmp/vpx-sdk/usr/include \
  -DVPX_LIBRARY=/tmp/vpx-sdk/usr/lib/x86_64-linux-gnu/libvpx.so
cmake --build build/live -j18 --target molink-core
# run with MOLINK_SURFACE=1 and the AV1 runtime libraries on the path
```

## Step 4 — packaging
- x64: MinGW64 MSVCRT toolchain (`tools/mingw64-msvcrt`), replace the MSVC
  Windows job once parity is reached.
- Win7 x86: mingw32 + libuvcpp 1.8.5 kit; GDI/D2D1 (no WebView2).
- Browser shell keeps WebView2 and ships x64 only.

## Open items
- Tabs wider than the strip → fold into a menu.
- DPI 150 %/200 % pass; listen for `WM_SETTINGCHANGE` to follow the system theme.
- Ship the stream-decoding core (dav1d/libvpx) in the Windows package; the
  Windows core still lacks the dav1d runtime.
- Native H.264 decoding still needs a hardware decoder (this host's VM has no
  GPU encoder at all).

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

## Build/link status (2026-10-31)

All native modules compile and link into one executable:

```
x86_64-w64-mingw32-windres -O coff -I . shell/windows/ui_kit_demo.rc \
  -o /tmp/ui-kit-demo-res.o
x86_64-w64-mingw32-g++ -std=c++20 -D_WIN32_WINNT=0x0A00 -O2 -mwindows \
  -municode -static -I shell/windows -o ui-full.exe \
  shell/windows/ui_kit_demo.cpp shell/windows/ui_kit.cpp \
  shell/windows/ws_client.cpp shell/windows/session_client.cpp \
  shell/windows/puff.c /tmp/ui-kit-demo-res.o \
  -ld2d1 -ldwrite -lole32 -luser32 -lgdi32 -ladvapi32 \
  -lwindowscodecs -lcomdlg32 -lws2_32 -lwinhttp \
  [-DMOLINK_HAVE_WEBP -I <libwebp>/src <libwebp>.a]
```

`MOLINK_HAVE_WEBP` enables device avatars: the host's `welcome` carries its
avatar (a `preset:*` key or an uploaded WebP data URL); the shell decodes the
WebP once with libwebp and caches a 32-bit BMP under
`%LOCALAPPDATA%\moLink\avatars`, then loads it with WIC. Without the define
(or when decoding fails) the card shows the initial letter instead.

- `ws_client` and `session_client` build at `_WIN32_WINNT=0x0601` (Win7 ok).
- The **demo** uses `GetDpiForWindow` / `SetProcessDpiAwarenessContext`, which
  need `0x0A00`; for the Win7 build those two calls must be resolved through
  `GetProcAddress` with a fallback.
- ~~Still to wire in the shell: start the core, read the ready line~~ — done
  2026-10-31: `moLink-native.exe` starts `molink-core.exe` at launch (the same
  ready-line handshake as the WebView2 shell, single-instance mutex
  `Local\moLinkShellInstance`, `--standalone` for test builds) and the home
  card reads `/api/device` + `/api/state` for the real directory id, code,
  address and online state. The old card was mock data (`BDV-NCK-9DN` /
  `482913` on every install), which is why several machines looked like the
  same registered device and why the directory had no such id.
- The session path (`session_client::start()`, `tick()` from the UI timer,
  `send_quality()`) is wired; the host core is now started once at launch and
  reused by the viewer path instead of lazily on the first connect.

## Tile mode and host settings (2026-10-31)

- **Tile clock (0x15).** The shell keeps per-128 px-cell capture timestamps and
  asks the host only for the cells it missed, at most once a second and at most
  64 rects (`session_client::handle_tile_clock`). The old "no frame for 700 ms →
  re-read the whole screen" poll is gone; a 5 s full-screen refresh remains only
  as a fallback when a host stops sending clocks.
- **Video mode.** The host's `video_mode` (stream / mixed / tiles) is in
  `welcome`, changes with `{"type":"video-mode","value":"..."}` (host-global,
  broadcast to every viewer and persisted), and the settings page shows the same
  three-way switch as the web's preferences panel. `tiles` is the static mode:
  only changed cells travel.
- **Security panel.** The toggles in 设置 → 安全 write the host's own settings
  through the loopback API (`POST /api/settings?allow_remote=...`), which needs
  the per-launch bearer token from the core's ready line (`token`). The shell
  reads it when it starts the core; a launcher attaching to a running core can
  pass `MOLINK_API_TOKEN`. Without it the panel keeps its defaults.
- **Session log.** Drops are appended to
  `%LOCALAPPDATA%\moLink\native-session.log` with the close code and reason
  (`close=1000: host restart`), which is the first file to read when a client
  reconnects on its own.
- Being controlled (被控) is still the installed core + service; the shell has
  no capture or listener of its own.

## Home / About polish (2026-10-31)

- Recents are de-duplicated by target on load (older `ui-recents.txt` files
  could hold the same host twice); a single click fills the address and code,
  a double click connects.
- About uses the product icon (extracted from the exe's icon resource through
  WIC) and its rows open the browser: repo, `ssvr.top/molink`, Star, and the
  release page.
- **Native update path (provisional):** the update button opens
  `https://github.com/Antruly/molink/releases/latest`; the native client only
  ever takes the `moLink-native` asset. The WebView2 bundle and the `.deb` are
  different products and must not be installed over this shell.
- The window icon now loads 32 px / 16 px frames separately (`hIconSm = hIcon`
  made Windows scale the 32 px frame down and look blurry).
