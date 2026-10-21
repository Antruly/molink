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
- 瓦片字节**不跨包**。一个瓦片必须完整落在同一帧包内。
- 表项顺序即数据顺序，无空洞、无尾部多余字节（解析器会拒绝）。

### 3.3 codec

| 值 | 名称 | 说明 |
|---:|---|---|
| 1 | `bgra` | 原始 32 位 BGRA，自上而下逐行；`byte_length` 必须 ≥ `w*h*4` |
| 2 | `png` | 单瓦片 PNG |
| 3 | `h264` | Annex-B 码流（规划中） |
| 4 | `av1` | OBU（规划中） |

`bgra` 的通道顺序是 **B、G、R、A**。Canvas `ImageData` 需要 R、G、B、A，渲染时必须换位。

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

令牌来自 `molink-core` 启动时的随机生成（32 位十六进制）或 `MOLINK_TOKEN` / `--token`。

### 4.2 控制消息

**服务端 → 客户端**

| type | 字段 | 说明 |
|---|---|---|
| `hello_required` | `protocol`, `service`, `version` | 等待鉴权 |
| `welcome` | `session`, `session_id`, `protocol`, `service`, `version`, `transport`, `tiles` | 鉴权成功 |
| `ping` | `t` | 应用层心跳，每 15 秒 |
| `pong` | `t`, `server_ms` | 对客户端 ping 的回应 |
| `error` | `code`, `message` | 错误 |

`welcome.tiles` 示例（真实屏幕）：

```json
{
  "codec": "bgra",
  "screen_w": 2880,
  "screen_h": 1920,
  "tile_w": 256,
  "tile_h": 256,
  "backend": "x11-shm",
  "synthetic": false
}
```

`backend` 取值：`x11-shm`、`x11`、`synthetic`。`synthetic: true` 表示当前是合成测试帧，
不是真实屏幕。

**客户端 → 服务端**

| type | 字段 | 说明 |
|---|---|---|
| `hello` | `token` | 鉴权 |
| `ping` | `t` | 客户端主动测延迟；服务端回 `pong` 并原样带回 `t` |
| `pong` | `t` | 回应服务端心跳 |
| `request-frame` | — | 请求一帧。服务端返回自上次以来所有未送达的帧；无变化时返回 0 瓦片帧 |
| `request-keyframe` | — | 请求下一个捕获帧标记为关键帧（客户端丢失基准时使用） |
| `input` | 事件字段 | 输入注入占位，当前回 `not_implemented` |
| `bye` | — | 正常关闭 |

### 4.3 帧送达与关键帧

- 帧包按会话独立编号（包头的 `seq`），每个响应至少包含一个帧包。
- **0 瓦片帧**表示"当前没有变化"，仍推进包序号，客户端不应重绘。
- 服务端保证：只要客户端收到过关键帧，之后收到的增量帧都是**按序**的；抓屏侧
  保留最近 120 帧 / 128 MiB 的历史，落后时逐帧补发。
- 历史被挤出或落后超过 60 帧时，服务端请求关键帧并发送 0 瓦片帧占位，直到关键帧
  到达。抓屏侧每 30 秒也会主动产生一次关键帧。
- 客户端在收到首个关键帧之前不应绘制任何增量帧。

### 4.4 错误码

| code | 含义 |
|---|---|
| `unauthorized` | 令牌缺失或错误 |
| `bad_json` | 控制消息不是 JSON 对象 |
| `unknown_type` | 不支持的控制消息类型 |
| `not_implemented` | 功能尚未实现（当前：输入注入） |
| `bad_packet` | 二进制消息不是合法 MOLK 包 |
| `frame_build_failed` | 服务端构造帧失败 |
| `send_failed` | 帧入队失败 |
| `unexpected_packet` | 该方向不接受此包类型 |

### 4.5 保活与回收

| 参数 | 值 | 行为 |
|---|---:|---|
| 心跳间隔 | 15 s | 服务端发 `ping`；客户端应回 `pong` |
| 鉴权超时 | 5 s | 未 hello 的连接以 `1008` 关闭 |
| 空闲回收 | 45 s | 无任何消息的连接以 `1001 "idle timeout"` 关闭 |

---

## 5. HTTP 接口

绑定地址默认 `127.0.0.1`，端口默认 `21118`。非回环地址需要 `--allow-remote` 明确确认。

| 方法 | 路径 | 鉴权 | 说明 |
|---|---|---|---|
| GET | `/healthz` | 否 | 健康检查，含 `service: "moLink Core"` |
| GET | `/api/health` | 否 | 同 `/healthz` |
| GET | `/api/info` | 是 | 版本、绑定地址、能力开关、会话数 |
| GET | `/api/sessions` | 是 | 当前会话快照 |
| GET | `/` | 否 | 服务自述 |

鉴权头（二者其一）：

```
Authorization: Bearer <token>
X-MoLink-Token: <token>
```

健康检查响应：

```json
{
  "status": "ok",
  "service": "moLink Core",
  "version": "0.1.0",
  "protocol": 1,
  "pid": 12345,
  "uptime_ms": 2032,
  "sessions": 0
}
```

CORS 只对以下来源开放：`tauri://localhost`、`http://tauri.localhost`、`https://tauri.localhost`、`http://localhost:1420`、`http://127.0.0.1:1420`。

---

## 6. 命令行与就绪行

```
molink-core [--host <addr>] [--port <n>] [--token <secret>]
            [--allow-remote] [--parent-pid <pid>] [--print-ready]
```

`--print-ready` 在开始监听后向 stdout 输出一行 JSON，壳层据此得到端口与令牌：

```json
{"event":"ready","service":"moLink Core","version":"0.1.0","protocol":1,
 "pid":12345,"host":"127.0.0.1","port":21118,"token":"<32 hex>",
 "session_endpoint":"/ws/session","synthetic_frames":true}
```

`--parent-pid` 让核心每秒检查一次父进程；父进程消失则自行退出，避免壳层被杀后留下孤儿进程。

---

## 7. 版本演进

- `kProtocolVersion` 只在**字节布局**变化时递增。
- 新增可选控制消息类型不需要递增协议版本；旧客户端应把未知 `type` 当作可忽略/报错处理。
- 新增包类型必须使用新值，不得复用被弃用的值。
- 修改 `codec` 编号、包头/帧头/瓦片表项布局，必须递增协议版本并同步更新 C++ 与 TS 两份实现和本文件。
