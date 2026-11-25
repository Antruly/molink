# 路线图：Web 端体验、远程终端、端口映射

记录时间：2026-11-25。当前版本 0.12.21。

这份文档只写**要做什么、为什么、怎么验收**；实现细节留给具体任务。

---

## 一、Web 端（浏览器直连）—— 我们独有的能力

### 1.1 视频流：为什么现在只有平铺

**现状**：Web 客户端在 hello 里声明 `h264_stream` / `vp9_stream` / `vp8_stream`（`frontend/src/lib/client.ts:1099-1101`），但每个标志只有在**真的解码成功一个探针关键帧之后**才置真（`client.ts:895` 注释与 `streamCodecDecodes`）。Chrome 自带 VP8/VP9 解码，所以问题不在浏览器，而在探针本身或编码器协商。

**要查的三处**
1. `vpx-stream.ts:24` 的 VP9 codec 串是 `vp09.00.10.08`——对 8-bit 4:2:0 profile 0 是对的；若宿主发的是 profile 2（10-bit）就会 `isConfigSupported` 通过但解码失败，探针失败后**永久降级为平铺**（`client.ts:3207` 里 `codec === 'vp9'` 直接把能力置 false，没有任何重试）。
2. 探针失败一次就永久关掉该编码，没有任何 TTL/重试；网络抖动或首帧丢失会被记成"不支持"。
3. 宿主侧是否真的会选择 VP9：`quality` 阶梯里 `ladder.front()` 的可用性取决于**所有** viewer 的能力，一个 viewer 声明不支持就全局退回。

**任务**
- W1 探针改为：失败后 30 秒重试一次，最多 3 次；失败原因写进诊断日志（`video.codec result=unsupported codec=vp9 reason=..`）。
- W2 编码串按实际 profile 生成（profile 0 / 2 分别对应 `vp09.00.10.08` / `vp09.02.10.10`）。
- W3 增加"强制编码"调试开关（URL 参数 `?codec=vp9`），绕过探针以便定位。
- **验收**：Chrome 连 Windows 宿主，界面显示"视频流"而不是"平铺"；日志里有 `video.codec result=ok codec=vp9`；拖动窗口流畅无马赛克。

### 1.2 Web 剪贴板

**现状**
- 文本：Web 端收发都走 `clipboard-sync`（`frontend/src/lib/clipboard-sync.ts`），需要页面处于聚焦状态才能读写（浏览器限制），已可用。
- 文件：**上传（本地→远端）可用**；**下载（远端→本地）有问题**，用户实测"从那边传过来下载到主控端好像有问题"。

**排查清单（按可能性排序）**
1. 浏览器下载路径分三支：File System Access（`showSaveFilePicker`）、`<a download>`、流式写本地 core（`/api/fs/write-chunk`）。大文件只有 FS Access 能流式写；若用户取消了选择器或浏览器不支持，会退到"先收完再 Blob"，内存爆掉或超时（`download-state.ts` / `FileTransferPanel.tsx:1908-1912`）。
2. `pull` 的续传节奏：每收一块发一次 `pull`，若 `handleChunk` 因 `writeLocalFileChunk` 失败（例如本地 core 的写接口有大小限制/句柄被复用）就会停在中途，表现为"下载不动"。
3. `armDownloadTimeout` 的超时值：慢盘/大文件时会在写盘期间误判超时并中止。
4. 文件名/路径：Windows 宿主的绝对路径不再下发（#74），浏览器侧若还依赖 `path` 就会退化成 `unknown.bin` 或写失败。

**任务**
- W4 给下载三条路径都加可观测日志（`download.mode=fs|blob|chunk bytes=.. wrote=..`），先拿到用户机器上的真实分支。
- W5 写本地 chunk 失败要**明确报错并保留已下载字节**，不要静默停在半途。
- W6 复现并修：目标 2 GB 文件在 Chrome/Edge 上成功率 100%，断点续传可用。
- **验收**：2.20 上选一个大目录下载到 Linux 主控端，逐个文件 sha256 一致。

### 1.3 Web 端的传输协议：WebSocket → WebRTC

**现状**：Web 走 WebSocket（HTTP/2 或 WebTransport，取决于服务端）；UDP 打洞只有原生壳有（`p2p/`）。WebRTC 能带来：原生 ICE 打洞、内置拥塞控制、更好的弱网表现。

**设计**
- 浏览器侧 `RTCPeerConnection` + DataChannel（控制/文件）+ MediaStream（视频，可选）。
- 信令复用现有 WS：`webrtc-offer` / `webrtc-answer` / `webrtc-ice`（原生壳已有 ICE 实现，可参照）。
- 传输层抽象：`SessionTransport` 接口已有（`backend/src/transport/session_transport.h`），新增 `webrtc_transport` 作为第四条路径（direct / relay / p2p-udp / webrtc）。
- 回退：WebRTC 失败自动回落 WebSocket，日志记原因。

**任务**（按依赖排序）
- W7 服务端 WebRTC 传输（libdatachannel 已在依赖里，`MOLINK_ENABLE_WEBRTC=ON` 已有构建开关）。
- W8 前端 `RTCPeerConnection` 接入 + 信令。
- W9 能力协商：hello 里声明 `webrtc: true`，宿主优先选它。
- **验收**：同一局域网内 Web 端连上后 `getStats()` 显示 candidate-pair 为 host/srflx 直连；拔掉 relay 仍能工作；弱网（`tc netem` 5% 丢包）下比 WS 更稳。

---

## 二、远程终端（新模块）

**定位**：与远程桌面、文件、聊天并列的第四个入口。连上之后点"远程终端"开一个终端窗口，行为等价于在对方机器上开一个 shell/命令提示符。

### 关键约束（用户明确）
- **以 molink 当前运行的用户身份执行**，不是 root/administrator 特权提升；有没有管理员权限取决于那个用户本身。
- 支持 Windows ↔ Linux 任意组合。
- 支持长任务（`tail -f`、编译、训练），断线不杀进程。
- 传输走 **TCP 或 QUIC**；UDP 打洞升级为 QUIC；中继同样支持。

### 设计
- **PTY**：POSIX 用 `forkpty`；Windows 用 **ConPTY**（Win10 1809+），老系统回退到管道 + `cmd.exe`（无真 PTY，明确提示）。
- **协议**：复用现有传输的二进制通道（`file_data` 那样的独立流），消息 `term-open` / `term-data`（原始字节）/ `term-resize` / `term-exit`。
- **会话持久化**：终端会话挂在**宿主侧**，与 viewer 连接解耦；断线重连后用 `term-attach` 拿回输出缓冲（环形 1 MiB）与仍在运行的进程。
- **多终端**：一个会话内可开多个终端（tab），各自 id。
- **安全**：沿用 #77 的能力位——新增 `terminal` capability，默认**关**，需要在设置里或按会话审批开启；每次开会话记 `term.open user=.. shell=..`。
- **限制**：不做 sudo/runas 提权；不提供"以 SYSTEM 运行"。

### 任务
- T1 宿主侧 PTY 抽象（`forkpty` / ConPTY）+ 单测（echo、resize、退出码）。
- T2 协议与传输接入（独立流，避免和文件抢带宽）。
- T3 viewer 侧终端组件（xterm.js；Web 与原生壳共用逻辑）。
- T4 断线重连 + 输出缓冲回放。
- T5 能力位与审计日志。
- **验收**：Linux 宿主从 Windows 客户端开终端跑 `htop`（全屏重绘正确）；Windows 宿主从 Linux 客户端跑 `dir` 与 PowerShell；断网 30 秒后重连，`ping` 还在跑且能看到中断前的输出。

---

## 三、端口映射（稳定版）

**参照**：RustDesk 的端口转发不稳定（用户实测）。我们要的是"一次配置、长期可靠"。

### 设计要点
- **TCP 优先**：本地监听 → 通过已有会话通道复用（`local_forward` 流），一个端口一条逻辑流，多路复用防头阻塞。
- **失败恢复**：本地监听进程独立于会话；会话重连后自动重建所有映射（`portmap.restore id=..`），不需要用户重开。
- **健康检查**：每条映射有 `connected / bytes / errors / last_error`，UI 里显示状态灯。
- **快捷映射**：内置预设 `RDP 3389`、`SSH 22`、`MySQL 3306`、`HTTP 80`、`HTTPS 443`、`SMB 445`、`VNC 5900`，一键添加（只填本地端口）。
- **权限**：映射由**宿主**授权（能力位 `port_forward`），默认关；开启后记录每次连接（源地址、目标端口、字节数）。
- **不做**：UDP 端口映射（第一版），需要时走 QUIC 数据报另议。

### 任务
- P1 映射管理器（宿主侧监听 + 会话复用 + 状态）。
- P2 断线重建与错误呈现。
- P3 快捷预设与 UI。
- P4 能力位与审计。
- **验收**：映射 3389 后用本机 `mstsc` 连 `127.0.0.1:13389` 直通宿主桌面；把宿主网络断 30 秒，恢复后**不用重配**即可再连；映射 3306 后 `mysql -h 127.0.0.1 -P 13306` 能连。

---

## 四、优先级与排期

| 顺序 | 内容 | 理由 |
|---|---|---|
| 1 | W1-W3 Web 视频流 | 浏览器是我们独有的入口，平铺观感差，且改动最小 |
| 2 | W4-W6 Web 下载修复 | 现网功能坏了，优先级高于新功能 |
| 3 | T1-T3 远程终端（MVP） | 用户明确要的新模块，独立于其它改动 |
| 4 | P1-P3 端口映射 | 依赖会话复用，排在终端之后可复用同一套流 |
| 5 | W7-W9 WebRTC | 收益大但改动最深，放在传输层稳定之后 |
| 6 | T4-T5、P4 收尾 | 持久化、权限、审计 |

## 五、不做 / 暂缓
- 提权执行（sudo / runas / SYSTEM）：与"以当前用户身份"的约定冲突。
- UDP 端口映射：等 QUIC 数据报通道成型。
- 移动端 Web 的终端键盘适配：先保证桌面浏览器。
