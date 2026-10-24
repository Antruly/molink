# moLink 线协议与接口契约

> 协议版本：**1**（`kProtocolVersion`）
> 对应实现：`backend/src/protocol/frame_packet.h`、`frontend/src/lib/protocol.ts`
> 状态：一阶段。真实抓屏、输入注入、UDP 直连与 WebRTC 尚未接入，但字节布局已经冻结。

本文是**唯一权威**的线上格式说明。C++ 与 TypeScript 两份实现都以本文件为准；两者不一致时是缺陷，不是选择。

---

## 1. 不变量

1. **Magic `"MOLK"` 不可更改。** 它在包头前 4 个字节，任何传输、任何版本都必须逐字节一致。
2. **所有整数小端序。** 与主机字节序无关。
3. **一条消息 = 一个完整包。** WebSocket 二进制消息、未来 UDP 数据报、RTCDataChannel 消息都承载恰好一条 MOLK 包；不做跨消息分片，也不在一条消息里放两条包。
4. **包头 + 载荷长度必须严格相等。** 接收方拒绝长度不匹配、尾部多余字节、瓦片越界的包。
5. 载荷上限 `64 MiB`（`kMaxPayloadBytes`），单帧瓦片数上限 `4096`。4K BGRA 关键帧约 33 MiB，上限必须高于它。

---

## 2. 包头（32 字节）

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | `magic` | `4D 4F 4C 4B`（"MOLK"） |
| 4 | 1 | `version` | 当前为 `1` |
| 5 | 1 | `type` | 包类型，见下表 |
| 6 | 2 | `flags` | 位标志，见 §2.1 |
| 8 | 4 | `session` | 后台分配的会话 id（自增 u32） |
| 12 | 4 | `seq` | 会话内包序号 |
| 16 | 8 | `ts_us` | 捕获时间戳，Unix 纪元微秒 |
| 24 | 4 | `payload_len` | 后续载荷字节数 |
| 28 | 4 | `reserved` | 必须为 0 |

### 2.1 包头 flags

| 位 | 名称 | 含义 |
|---:|---|---|
| 0 | `KEYFRAME` | 本包含关键帧（整帧可独立解码） |
| 1 | `COMPRESSED` | 载荷经过压缩（当前未使用） |
| 2 | `LAST` | 控制流中的最后一条 |

### 2.2 包类型

| 值 | 名称 | 载荷 |
|---:|---|---|
| `0x01` | `HELLO` | JSON 控制载荷（传输层握手） |
| `0x02` | `FRAME` | 帧体，见 §3 |
| `0x03` | `ACK` | JSON |
| `0x10` | `INPUT` | JSON 输入事件批（当前回 `not_implemented`） |
| `0x11` | `CLIPBOARD` | JSON |
| `0x12` | `FILE_DATA` | `u32 id` + `u32 保留` + `u64 offset` + 文件字节 |
| `0x13` | `AUDIO` | 交错 little-endian 16-bit PCM（格式见 welcome） |
| `0x14` | `VOICE` | 语音通话 PCM：48 kHz 单声道 little-endian 16-bit |
| `0x7F` | `CONTROL` | 会话级 JSON 控制 |
| `0x80` | `PING` | 空 |
| `0x81` | `PONG` | 空 |

未知类型必须被忽略或回错，**不得**按已知类型解析。

---

## 3. FRAME 载荷

帧体 = 16 字节帧头 + N × 16 字节瓦片表项 + 按表项顺序拼接的瓦片字节。

### 3.1 帧头

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | `frame_seq` | 帧序号 |
| 4 | 2 | `tile_count` | 瓦片数（≤ 4096） |
| 6 | 1 | `codec` | 见 §3.3 |
| 7 | 1 | `flags` | 帧标志 |
| 8 | 2 | `screen_w` | 屏幕宽（像素） |
| 10 | 2 | `screen_h` | 屏幕高（像素） |
| 12 | 2 | `tile_w` | 标称瓦片宽 |
| 14 | 2 | `tile_h` | 标称瓦片高 |

帧 flags：

| 位 | 名称 | 含义 |
|---:|---|---|
| 0 | `KEYFRAME` | 整帧可独立解码 |
| 1 | `PARTIAL` | 只包含变化瓦片；未列出的区域保持上一帧内容 |

### 3.2 瓦片表项（每个 16 字节）

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 2 | `x` | 左边界（像素） |
| 2 | 2 | `y` | 上边界（像素） |
| 4 | 2 | `w` | 宽 |
| 6 | 2 | `h` | 高 |
| 8 | 1 | `flags` | 瓦片标志 |
| 9 | 3 | `reserved` | 0 |
| 12 | 4 | `byte_length` | 紧随其后的瓦片字节数 |

瓦片 flags：

| 位 | 名称 | 含义 |
|---:|---|---|
| 0 | `CHANGED` | 本帧有变化 |
| 1 | `KEYFRAME` | 该瓦片可独立解码 |

约束：

- 瓦片矩形必须落在 `screen_w × screen_h` 内；接收方应做边界检查。
- 瓦片尺寸不要求等于 `tile_w × tile_h`：服务端会把同一行里连续变化的瓦片合并成一条完整的水平条带（关键帧因此是每行一条），接收方必须按表项中的 `w/h` 处理任意矩形。
- 瓦片字节**不跨包**。一个瓦片必须完整落在同一帧包内。
- 表项顺序即数据顺序，无空洞、无尾部多余字节（解析器会拒绝）。

### 3.3 codec

| 值 | 名称 | 说明 |
|---:|---|---|
| 1 | `bgra` | 原始 32 位 BGRA，自上而下逐行；`byte_length` 必须 ≥ `w*h*4` |
| 2 | `png` | 单瓦片 PNG |
| 3 | `h264` | Annex-B 码流（规划中） |
| 4 | `av1` | OBU（规划中） |
| 5 | `deflate_bgra` | zlib(deflate) 压缩的 BGRA 瓦片 |
| 6 | `rgb24` | 原始 24 位 RGB（R、G、B），丢弃 alpha |
| 7 | `rgb565` | 原始 16 位 RGB565，小端 |
| 8 | `deflate_rgb24` | zlib(deflate) 压缩的 RGB24 瓦片 |
| 9 | `deflate_rgb565` | zlib(deflate) 压缩的 RGB565 瓦片 |

`bgra` 的通道顺序是 **B、G、R、A**。Canvas `ImageData` 需要 R、G、B、A，渲染时必须换位。
`rgb24` 的顺序是 **R、G、B**；`rgb565` 每像素 2 字节、小端，位布局为
`RRRRRGGG GGGBBBBB`（5/6/5），解码时用位复制扩展回 8 位。

### 3.4 瓦片压缩（codec 5 / 8 / 9）

- `codec = 5/8/9` 时，每个瓦片的 `byte_length` 是 zlib 流（带 zlib 头，浏览器
  `DecompressionStream("deflate")` 可直接解压）的长度。
- 解压后的长度必须按 codec 精确匹配：`bgra` 为 `w*h*4`，`rgb24` 为 `w*h*3`，
  `rgb565` 为 `w*h*2`。
- 服务端按画质预设选择 zlib 级别：关键帧/大突发在“流畅”档固定级别 1，
  “平衡”档沿用自适应（1/3/6），“清晰”档提高到 3/6/9。
- 如果整帧压缩后不小于该格式的原始大小，服务端回退为对应的原始格式
  （`1/6/7`）。有损格式**不会**因为有损而回退到 BGRA：打包后的 2/3 字节
  仍然比 4 字节小。
- 浏览器原生只支持 `DecompressionStream("deflate")`，所以服务端不直接发 Brotli；桌面客户端如需要 Brotli，应在传输协商中单独约定 codec，而不是复用 codec 5。
- 帧内所有瓦片使用同一种 codec，不混合。

### 3.5 画质预设与有损格式

`quality` 控制消息（见 §4.2）把主机编码切换到以下预设之一：

| 预设 | 格式 | zlib | 变化阈值 | 用途 |
|---|---|---|---|---|
| `smooth`（流畅） | `rgb565` | 固定 1 | 至少 16 | 优先帧率与包体积；16 位色，丢弃细小的颜色差异 |
| `balanced`（平衡，默认） | `bgra` | 自适应 | 使用配置值（默认 4） | 无损，和旧版本行为一致 |
| `quality`（清晰） | `bgra` | 3/6/9 | 最多 1 | 优先画质，加大压缩力度 |
| `raw`（原画不压缩） | `bgra` | 关闭 | 最多 1 | 预留：客户端可显式请求；1080p/30fps 的裸 BGRA 流量远超普通局域网，UI 不暴露 |
| `game`（游戏） | `rgb565` | 固定 1 | 至少 16 | 60fps 优先，允许有损；等价于 `smooth` |

`ultra`（超清）和 `lossless`（无损）映射到 `quality`：BGRA 原画 + zlib（无损，
不是不压缩），客户端用保留的 `fps` 区分 15fps 与 30fps；`game` 映射到 `smooth`
（16 位色 + 快速 zlib）。`raw` 只保留给显式请求不压缩的客户端。预设是**主机全局**的，与采集帧率相同：一个会话切换会影响同一主机
上的所有会话。`auto` 不是 wire 协议的一部分，由客户端根据 RTT、拉帧完成率和吞吐量
动态调整帧率（最高 30，稳定在 15 左右）和画质预设，并带迟滞。

---

## 4. WebSocket 通道

- 端点：`ws://{host}:{port}/ws/session`
- 二进制消息：一条 MOLK 包。
- 文本消息：UTF-8 JSON 控制消息。
- 单条消息上限：接收方向 1 MiB（控制消息足够）。

### 4.1 鉴权

鉴权在应用层完成，不在握手层：

1. 连接建立后，服务端发 `hello_required`（或直接发 `welcome`，若 URL 带有效 `?token=`）。
2. 客户端发送：

```json
{"type":"hello","token":"<token>","client":"molink-web","protocol":1}
```

3. 令牌正确 → `welcome`；错误 → `error` 后以 close code `1008` 关闭。
4. 连接后 5 秒内未完成鉴权 → 服务端以 `1008 "hello timeout"` 关闭。
5. 错误令牌按来源 IP 计数，与 `/api/login` 共用阶梯封禁额度（见 §5.1）；封禁中的来源在握手后立即以 `1008` 关闭。

令牌来自 `molink-core` 启动时的随机生成（32 位十六进制）或 `MOLINK_TOKEN` / `--token`。

### 4.1.1 免密审批

没有验证码的客户端可以请求被控端手动同意：

```json
{"type":"request-access","name":"Alice 的浏览器","client":"molink-web"}
```

服务端回 `{"type":"access-requested","id":N}`，连接保持打开（不受 5 秒
`hello timeout` 限制，审批等待上限 120 秒）。被控端在本机 UI 上允许后，
服务端直接发 `welcome`，客户端无需再发 `hello`；拒绝则回 `error`
（`code=access_denied`）并关闭。开关关闭时回 `approval_disabled`。

本机 UI 通过 `/api/state` 的 `access_requests` 字段（仅回环地址返回）轮询
待审批列表，通过 `POST /api/access/approve?id=N` 或 `/deny?id=N` 决定。
「设置 → 安全 → 允许免密远程请求」对应 `/api/settings?access_requests=0|1`，
默认开启。锁定状态下请求会被直接拒绝。

### 4.2 控制消息

**服务端 → 客户端**

| type | 字段 | 说明 |
|---|---|---|
| `hello_required` | `protocol`, `service`, `version` | 等待鉴权 |
| `welcome` | `session`, `session_id`, `protocol`, `service`, `version`, `transport`, `tiles`, `displays`, `display_index` | 鉴权成功；`displays` 为可切换显示器列表 |
| `ping` | `t` | 应用层心跳，每 15 秒 |
| `pong` | `t`, `server_ms` | 对客户端 ping 的回应 |
| `access-requested` | `id` | 已登记免密请求，等待本机用户审批 |
| `error` | `code`, `message` | 错误 |

`welcome` 示例（真实屏幕）：

```json
{
  "type": "welcome",
  "session": "A3F-9K2-X7Q",
  "session_id": 1,
  "protocol": 1,
  "service": "moLink Core",
  "version": "0.8.0",
  "transport": "websocket",
  "tiles": {
    "codec": "deflate-bgra",
    "screen_w": 2880,
    "screen_h": 1920,
    "tile_w": 256,
    "tile_h": 256,
    "backend": "x11-shm",
    "synthetic": false
  },
  "input": true,
  "input_backend": "xtest",
  "clipboard": true,
  "clipboard_backend": "x11-selection"
}
```

`backend` 取值：`x11-shm`、`x11`、`synthetic`。`synthetic: true` 表示当前是合成测试帧，
不是真实屏幕。`codec` 反映当前帧实际使用的编码（`bgra`、`deflate-bgra`、`rgb24`、
`rgb565`、`deflate-rgb24` 或 `deflate-rgb565`）；`quality` 是当前画质预设
（`smooth`/`balanced`/`quality`/`raw`）；`input` 表示本机是否支持输入注入；
`clipboard` 表示本机是否支持剪贴板同步。

**客户端 → 服务端**

| type | 字段 | 说明 |
|---|---|---|
| `hello` | `token` | 鉴权 |
| `request-access` | `name`, `client` | 无验证码请求远程访问；需被控端开关开启 |
| `ping` | `t` | 客户端主动测延迟；服务端回 `pong` 并原样带回 `t` |
| `pong` | `t` | 回应服务端心跳 |
| `request-frame` | — | 请求一帧。服务端返回自上次以来所有未送达的帧；无变化时返回 0 瓦片帧 |
| `request-keyframe` | — | 请求下一个捕获帧标记为关键帧（客户端丢失基准时使用） |
| `capture-fps` | `fps` (1..60) | 调整主机的采集帧率（全局）；服务端回 `capture-fps` |
| `quality` | `mode`（`smooth`/`balanced`/`quality`/`ultra`/`lossless`/`raw`/`game`），可选 `fps` | 切换主机的编码预设（全局）；`fps` 同时调整采集帧率（为兼容旧客户端保留）。服务端回 `quality`，带实际生效的 `mode` 与 `format` |
| `display` | `index` | 切换被控端显示器（全局）；服务端回 `display` 并发送新关键帧 |
| `input` | `events[]` | 指针/键盘事件；需要已鉴权，见 §4.3 |
| `bye` | — | 正常关闭 |

### 4.2.1 显示器选择

`welcome.displays` 的每一项包含 `index`、`name`、`x`、`y`、`width`、`height`、`primary`；`display_index` 是当前采集的显示器，默认 `0`（主显示器）。客户端发送 `{"type":"display","index":N}` 后，服务端切换采集、按新显示器重算输入坐标，并发布关键帧。显示器列表是主机级共享的：一个会话切换会影响同一主机上的其他会话。

### 4.3 输入事件

```json
{
  "type": "input",
  "events": [
    {"k": "move", "x": 0.42, "y": 0.31},
    {"k": "button", "button": 1, "down": true},
    {"k": "button", "button": 1, "down": false},
    {"k": "wheel", "dx": 0, "dy": 1},
    {"k": "key", "code": "KeyA", "down": true},
    {"k": "key", "code": "KeyA", "down": false}
  ]
}
```

| 字段 | 说明 |
|---|---|
| `k` | `move` / `button` / `wheel` / `key` |
| `x`, `y` | 归一化到 `[0,1]` 的屏幕坐标；越界事件被拒绝 |
| `button` | X11 按钮号：1 左、2 中、3 右（4–7 为滚轮，由服务端使用） |
| `dx`, `dy` | 滚轮格数；`dy > 0` 向下滚动 |
| `code` | 浏览器 `KeyboardEvent.code`，如 `KeyA`、`Digit1`、`ArrowLeft`、`ShiftLeft` |
| `down` | 按下/抬起，默认 `true` |

- 单条消息最多 64 个事件。
- `events` 缺失时，消息顶层对象本身按一个事件解析。
- 组合键由客户端分别发送修饰键与主键的按下/抬起，X 服务器负责产生大写与符号。
- 会话断开且没有其它已鉴权会话时，服务端释放所有按下的键与按钮。
- 输入不可用时返回 `not_implemented`。

### 4.4 光标

X11 的屏幕抓取**不包含鼠标光标**，服务端单独发送形状与位置：

```json
{"type":"cursor","serial":300,"w":48,"h":48,"hotspotX":7,"hotspotY":7,
 "rgba":"<base64>"}
{"type":"cursor-position","x":1617,"y":1006,"visible":true}
```

| 字段 | 说明 |
|---|---|
| `serial` | 光标形状序号；序号变化时才重发 `cursor` |
| `w`, `h` | 光标像素尺寸 |
| `hotspotX`, `hotspotY` | 热点（光标坐标对应的像素） |
| `rgba` | `w*h*4` 字节的 RGBA（非预乘），base64 |
| `x`, `y` | 指针在屏幕坐标系中的位置 |
| `visible` | 为 false 时客户端应隐藏光标 |

- 两条消息随 `request-frame` 的响应发送；形状只在变化时发送。
- 客户端在控制模式开启时通常隐藏该覆盖层，改由本地光标指示位置。

### 4.5 剪贴板

**客户端 → 服务端**

```json
{"type":"clipboard","action":"set","text":"hello"}
{"type":"clipboard","action":"get"}
```

**服务端 → 客户端**

```json
{"type":"clipboard","text":"hello from the remote machine"}
```

- 仅支持文本（UTF8_STRING），单条上限 192 KiB。
- `set` 让服务端取得 X11 CLIPBOARD 所有权，本地应用随后粘贴即可得到该文本。
- `get` 异步请求当前所有者；没有所有者或超时返回空字符串。
- 剪贴板不可用时返回 `not_implemented`。

### 4.6 帧送达与关键帧

- 帧包按会话独立编号（包头的 `seq`），每个响应至少包含一个帧包。
- **0 瓦片帧**表示"当前没有变化"，仍推进包序号，客户端不应重绘。
- 服务端保证：只要客户端收到过关键帧，之后收到的增量帧都是**按序**的；抓屏侧
  保留最近 120 帧 / 128 MiB 的历史，落后时逐帧补发。
- 历史被挤出或落后超过 60 帧时，服务端请求关键帧并发送 0 瓦片帧占位，直到关键帧
  到达。抓屏侧每 30 秒也会主动产生一次关键帧。
- 客户端在收到首个关键帧之前不应绘制任何增量帧。

### 4.7 错误码

| code | 含义 |
|---|---|
| `unauthorized` | 令牌缺失或错误 |
| `bad_json` | 控制消息不是 JSON 对象 |
| `unknown_type` | 不支持的控制消息类型 |
| `not_implemented` | 功能不可用（本机没有输入注入能力） |
| `bad_input` | 输入事件字段缺失或越界 |
| `bad_clipboard` | `action` 不是 `set`/`get` |
| `clipboard_too_large` | 剪贴板文本超过 192 KiB |
| `bad_packet` | 二进制消息不是合法 MOLK 包 |
| `frame_build_failed` | 服务端构造帧失败 |
| `send_failed` | 帧入队失败 |
| `unexpected_packet` | 该方向不接受此包类型 |

### 4.8 保活与回收

| 参数 | 值 | 行为 |
|---|---:|---|
| 心跳间隔 | 15 s | 服务端发 `ping`；客户端应回 `pong` |
| 鉴权超时 | 5 s | 未 hello 的连接以 `1008` 关闭 |
| 空闲回收 | 5 min | 无任何消息的连接以 `1001 "idle timeout"` 关闭 |

### 4.9 聊天、文件与音频

以下消息都要求会话已鉴权（hello 成功或初始 token 有效）。

**聊天**

- 客户端 → 核心：`{"type":"chat","action":"send","text":"..."}`（1–4096 字节）
- 核心 → 所有会话与主机 UI：`{"type":"chat","action":"message","id":N,"from":"remote|host","text":"...","ts":epoch_ms}`
- 客户端请求历史：`{"type":"chat","action":"history"}` → `{"type":"chat","action":"history","messages":[...]}`
- 主机 UI 通过 `GET /api/chat?since=N` 轮询、`POST /api/chat?text=...` 回复（仅回环）。

**文件传输**

- 上传：`{"type":"file","action":"offer","name":"a.txt","size":123}` →
  `{"type":"file","action":"accept","id":N,"chunk":262144,"dir":"..."}`；随后用包类型
  `0x12` 按 offset 顺序发送数据，最后 `{"action":"complete","id":N}` →
  `{"action":"done","id":N,"path":"..."}`。文件保存在远程主机 `~/Downloads/moLink`。
- 浏览：`{"action":"browse","path":""}` → `{"action":"listing","path":"...","root":"...","entries":[{"name":"a","dir":false,"size":1,"mtime":0}]}`；
  空路径表示主目录，路径被限制在主目录内（含符号链接解析）。
- 下载：`{"action":"request","path":"..."}` → `{"action":"sending","id":N,"name":"a","size":123}`；
  每收到一块后发送 `{"action":"pull","id":N}` 拉取下一块，核心用 `0x12` 回传，最后
  `{"action":"eof","id":N}`。分块 256 KiB。
- 远程剪贴板：`{"type":"clipboard","action":"files"}` 查询当前文件列表；主机剪贴板变化时核心
  主动推送 `{"type":"clipboard","action":"files","files":[{"name","path","size"}]}`。
  `{"type":"file","action":"clipboard-set","paths":[...]}` 把远程文件放进主机剪贴板
  （Windows `CF_HDROP`），回复 `{"type":"file","action":"clipboard","ok":true,"count":N}`。

**音频**

- 客户端 → 核心：`{"type":"audio","action":"start"}` / `{"type":"audio","action":"stop"}`；
  start 回复 `{"type":"audio","action":"started","sample_rate":48000,"channels":2,"bits":16}`。
- welcome 中 `audio` 表示能力，`audio_format` 给出 `sample_rate` / `channels` / `bits`。
  核心用包类型 `0x13` 发送交错 16-bit PCM。当前实现为 Windows WASAPI 回环采集；其他平台
  `audio=false`。

**语音通话（不含视频）**

- 客户端发起：`{"type":"call","action":"invite"}`；核心回推事件
  `{"type":"call","action":"ringing|accepted|declined|ended|audio_error","host_microphone":bool,"started_ms":epoch_ms}`。
- 主机端通过 `GET /api/call` 轮询状态（`idle|ringing|active`），用
  `POST /api/call?action=accept|decline|end` 接听/拒绝/挂断（仅回环）。
- 客户端挂断：`{"type":"call","action":"hangup"}`。
- 通话音频使用包类型 `0x14`，载荷为 48 kHz 单声道 little-endian 16-bit PCM；
  客户端发麦克风，主机发本机麦克风。welcome 的 `call` 表示该主机是否支持通话。
  当前实现为 Windows WASAPI；其他平台 `call=false`。

---

## 5. HTTP 接口

核心监听 `0.0.0.0:28418`，但 `allow_remote_access` 默认关闭：非回环 TCP 连接在 HTTP 解析前立即关闭，已有远程 WebSocket 也会断开。打开该设置后立即生效，无需重启；显式 `--host 127.0.0.1` 则真正只绑定本机。非回环浏览器还需要 `allow_web_client`，且必须通过授权码登录。

| 方法 | 路径 | 鉴权 | 说明 |
|---|---|---|---|
| GET | `/healthz` | 否 | 健康检查，含 `service: "moLink Core"` |
| GET | `/api/health` | 否 | 同 `/healthz` |
| GET | `/api/state` | 否 | 本机/远程、锁定状态、设备 ID、锁定超时、全部端口 |
| GET | `/api/transports` | 否 | 传输协商表：可用性、端口、推荐顺序 |
| POST | `/api/login` | 否 | 用长期/临时授权码登录/解锁；返回 `session_token`；失败限速 |
| GET | `/api/device` | 仅回环 | 设备 ID、本机会话 token、临时码状态；锁定时 423 |
| GET | `/api/discovery` | 是 + 仅回环 | 局域网发现的设备列表 |
| GET | `/api/settings` | 是 + 仅回环 | 安全设置与长期授权码 |
| POST | `/api/settings?allow_remote=&allow_web=&discovery=&name=` | 是 + 仅回环 | 修改安全设置 |
| POST | `/api/auth/code?mode=&uses=&value=` | 是 + 仅回环 | 切换/刷新单次或多次临时码 |
| POST | `/api/auth/long-term?value=` | 是 + 仅回环 | 设置/生成长期授权码 |
| GET | `/api/info` | 是 | 版本、绑定地址、能力开关、会话数 |
| GET | `/api/sessions` | 是 | 当前会话快照 |
| POST | `/api/activity` | 是 | 记录一次用户活动 |
| POST | `/api/ui-state?client=&sessions=` | 是 | 上报某窗口打开的会话数 |
| POST | `/api/lock-timeout?seconds=` | 是 | 修改并持久化空闲锁定时间（0 = 从不） |
| POST | `/api/network?tcp=&udp=` | 是 | 修改并持久化 TCP/UDP 端口；重启后生效 |
| GET | `/` | 否 | 配置了 `--web-root` 时返回前端 `index.html`，否则返回服务自述 JSON |

鉴权头（二者其一）使用 `/api/login` 返回的会话 token（不是授权码）：

```
Authorization: Bearer <session_token>
X-MoLink-Token: <session_token>
```

桌面壳通过回环 `/api/device` 获取自己的会话 token。WebSocket 的 `hello` / `?token=` 可以携带会话 token 或授权码；代码成功鉴权后会按单次/多次规则消耗。

健康检查响应：

```json
{
  "status": "ok",
  "service": "moLink Core",
  "version": "0.8.0",
  "protocol": 1,
  "pid": 12345,
  "uptime_ms": 2032,
  "sessions": 0
}
```

CORS 只对本地开发来源 `http://localhost:1420`、`http://127.0.0.1:1420` 开放；核心托管的 UI 与接口同源，不需要 CORS。

### 5.1 访问模型与锁定

- `peer` 为回环地址（`127.0.0.1`、`::1`、`::ffff:127.0.0.1`）时视为本机访问，可免验证读取 `/api/device`。
- 默认只绑定回环；`allow_remote_access` 打开后才接受非回环连接，`allow_web_client` 控制非回环浏览器能否加载 UI/登录页。
- 其他来源必须用授权码登录（`POST /api/login`）；成功后返回 `session_token`，后续 API 使用该 token。授权码分长期码、单次码、多次码：长期码始终有效；单次码用一次后自动换新；多次码按次数扣减，用完后自动换新。
- 没有已鉴权 WebSocket 会话、且所有 UI 窗口上报的 `sessions` 为 0、且空闲超过 `lock_timeout` 秒时，核心锁定。
- 锁定期间：除 `/api/state`、`/api/login`、`/api/health` 外的 `/api/*` 返回 `423 locked`；WS `hello` 以 `1008 locked` 关闭。
- 验证失败按来源 IP 计数，`/api/login`、其他 `/api/*` 的错误会话 token 与 WebSocket `hello` 共用额度：每轮 5 次失败后锁定，时长依次为 30 秒、2 分钟、10 分钟、1 小时、6 小时；第 6 轮永久封禁。24 小时无新失败重置阶梯；回环地址不计数。
- 临时锁定返回 `429` 与 `Retry-After`（JSON 中同时带 `retry_after`），永久封禁返回 `403 ip_banned`。封禁列表保存在配置目录的 `banned_ips`，删除后重启核心即可解除。

### 5.2 传输协商

`GET /api/transports`（无需鉴权，锁定期间也可用）返回当前 build 的传输能力和端口表：

```json
{
  "current": "websocket",
  "preferred": ["quic","http3","tcp","http2","websocket","udp"],
  "ports": {"web":28418,"http3":28419,"quic":28420,"tcp_data":28421,"udp_data":28422},
  "transports": [
    {"name":"websocket","role":"web","available":true,"secure":false,
     "port":28418,"note":"browser baseline; /ws/session"},
    {"name":"http2","role":"api","available":false,"secure":false,
     "port":28418,"note":"h2c prior knowledge on the web port"},
    {"name":"http3","role":"api","available":false,"secure":true,
     "port":28419,"note":"needs TLS + QUIC front-end"},
    {"name":"quic","role":"data","available":false,"secure":true,
     "port":28420,"note":"direct data channel (planned)"},
    {"name":"tcp","role":"data","available":false,"secure":false,
     "port":28421,"note":"direct data channel (planned)"},
    {"name":"udp","role":"data","available":false,"secure":false,
     "port":28422,"note":"direct data channel (planned)"}
  ]
}
```

- 选择规则：客户端按 `preferred` 顺序取第一个自己支持且 `available: true` 的项；当前所有客户端都会落到 `websocket`。
- WebSocket `welcome` 同时带 `transport`、`transports` 与 `transport_ports`，已连接的客户端不需要再走一次 HTTP。
- 浏览器 WebSocket API 基于 HTTP/1.1，h2/h3 不会替代它；h2c 只对桌面/命令行客户端有意义，HTTP/3 需要 TLS 证书与独立 QUIC 前端。
- 直连数据通道（QUIC/TCP/UDP）接入后只把对应条目的 `available` 置真，协商字段和端口表不变。

---

## 6. 命令行与就绪行

```
molink-core [--host <addr>] [--port <n>] [--http3-port <n>] [--quic-port <n>]
            [--tcp-data-port <n>] [--udp-port <n>] [--token <secret>]
            [--allow-remote] [--parent-pid <pid>] [--print-ready]
```

`--host` 默认 `0.0.0.0`。端口段：`28418` 网页/API/WS、`28419` HTTP/3、
`28420` QUIC 数据、`28421` TCP 数据、`28422` UDP 数据。命令行参数优先于
`/api/network` 与 `/api/lock-timeout` 写入配置目录的持久化值。

`--print-ready` 在开始监听后向 stdout 输出一行 JSON，壳层据此得到端口与令牌：

```json
{"event":"ready","service":"moLink Core","version":"0.8.0","protocol":1,
 "pid":12345,"host":"127.0.0.1","port":28418,"token":"<32 hex>",
 "client_token":"<32 hex>","session_endpoint":"/ws/session",
 "transport":"websocket",
 "transports":["websocket"],
 "ports":{"web":28418,"http3":28419,"quic":28420,
          "tcp_data":28421,"udp_data":28422},
 "synthetic_frames":true}
```

`token` 是只给本机壳层的会话 token，不是授权码；授权码通过 `/api/device` / 设置页展示。

`client_token` 只通过 ready 行交给壳层。`allow_web_client=0` 时，回环 Web UI、`/api/state`、`/api/login`、`/api/device`、`/api/transports` 需要请求头 `X-MoLink-Client` 或初始 URL 的 `client=`；WebSocket 不受这道门限制（浏览器 WS 不能加自定义头），仍必须用授权码或会话 token 鉴权。

`--parent-pid` 让核心每秒检查一次父进程；父进程消失则自行退出，避免壳层被杀后留下孤儿进程。

---

## 7. 版本演进

- `kProtocolVersion` 只在**字节布局**变化时递增。
- 新增可选控制消息类型不需要递增协议版本；旧客户端应把未知 `type` 当作可忽略/报错处理。
- 新增包类型必须使用新值，不得复用被弃用的值。
- 修改 `codec` 编号、包头/帧头/瓦片表项布局，必须递增协议版本并同步更新 C++ 与 TS 两份实现和本文件。

---

## 8. 二阶段设备目录（moLink Server）

桌面核心在设置中配置 `server_url` / `server_invite` 后，会向目录服务注册并每 30 秒发送心跳；
服务端 API 见独立仓库
[Antruly/molink-server](https://github.com/Antruly/molink-server)。核心侧行为：

- 配置保存在 `~/.config/molink/settings` 的 `server_url`、`server_invite`、
  `server_enabled`，核心线程每秒轮询，修改后即时生效。`server_url` 为空且总开关开启时
  使用内置目录 `https://molink.ssvr.top`（`MOLINK_DEFAULT_SERVER_URL`）。
- 注册得到的凭据保存在 `~/.config/molink/server.json`（`url`、`device_id`、`display_id`、
  `token`、`domain`、`domain_valid`，POSIX 下权限 0600）；重启后复用同一 ID，心跳失败
  （401/404）才重新注册。
- `GET /api/server`（仅回环）：`url`、`custom_url`、`default_url`、`using_default`、
  `invite`、`enabled`、`registered`、`online`、`device_id`、`display_id`、`domain`、
  `domain_valid`、`last_error`。
- `POST /api/server?url=&invite=&enabled=`（仅回环）：保存配置并触发重新注册；`enabled`
  独立于自定义 URL，可只关远程目录而保留地址。
- `POST /api/domain`（仅回环，JSON `{action, code?}`）：`join` 加入他设备的域组码，
  `rotate` 让目录为本机签发新码（本机是旧码所有者时旧码立即失效）。
- `GET /api/domain/devices`（仅回环）：返回同域组设备列表；码已失效时返回
  `unknown_domain`。
- `GET /api/state` 增加 `device_name`、`avatar`、`platform` 与 `server` 对象（字段同上，
  但不含 token/invite）。
- 通过设备 ID 连接：客户端使用已配置的 server URL 请求
  `GET <server>/v1/devices?id=<ID>`，拿到 `address`/`peer_ip`/`port`/`name` 后按正常的直连
  会话流程鉴权；目录只做寻址，不转发媒体。
- 目录客户端支持 `https://`：使用系统信任库，或在设置中提供 `server_ca`（PEM 路径）
  以信任私有 CA。服务端用 `--tls-cert/--tls-key` 开启 TLS，TLS 上只宣告 HTTP/1.1。
