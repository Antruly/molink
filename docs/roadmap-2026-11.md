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

**参照**：同类产品的端口转发不稳定（用户实测）。我们要的是"一次配置、长期可靠"。

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
| 3 | H1-H2 被控端专用版（core 模式 + Web 分支） | 产品形态需求，先打通"只能被控"，UI 可以随后 |
| 4 | H3-H5 被控端专用版（D2D 壳 + 安装包） | 依赖 H1/H2 |
| 5 | T1-T3 远程终端（MVP） | 用户明确要的新模块，独立于其它改动 |
| 6 | P1-P3 端口映射 | 依赖会话复用，排在终端之后可复用同一套流 |
| 7 | M1-M3 移动端阶段 A（WebView 壳） | 复用现有 Web UI，最快拿到可用 APK |
| 8 | M4 移动端阶段 B（原生解码） | 低延迟，依赖阶段 A 的会话层 |
| 9 | Y1-Y2 Wayland（检测提示 + portal 采集） | 先让用户知道现状，再接采集 |
| 10 | Y3-Y5 Wayland（输入/剪贴板/组合键） | 依赖采集跑通 |
| 11 | W7-W9 WebRTC | 收益大但改动最深，放在传输层稳定之后 |
| 12 | T4-T5、P4、M6 收尾 | 持久化、权限、审计、手机被控 |

## 五、libuvcpp 升级到 1.11.0（计划，不自行编译）

官方已发布 **v1.11.0**（2026-11-23），六个平台都有预编译包，**用官方二进制，不自己编译**。

### 为什么值得升
- **`uvcpp_service_run(app, argc, argv)`**：一行接入 Windows 服务 install / uninstall / start / stop / status（SCM、失败自动重启）、SIGINT/SIGTERM 优雅停机、工作目录与环境变量、stdout/stderr 重定向与轮转、单实例互斥。
  我们现在这套服务是**手写的**（`shell/windows/service/main.cpp` + `service_control.cpp` 的命名管道 + 安装脚本 `sc create` + 计划任务），前几天 0.12.20 "控制不了"就是服务与用户会话两个 core 抢端口造成的 —— 换成官方实现能直接消掉这一类问题。
- **`uvcpp_login_limiter`**：失败窗口/锁定/审计，可以替换 `lock_state.cpp` 里手写的限流。
- `uvcpp_ws_bus`（Redis 跨进程广播）、`uvcpp_db_job_store`：当前单机单 core 用不上，先不接。

### 现状
- CI 与打包：默认 `1.11.0`（`packaging/fetch-uvcpp.sh msvc-x64 1.11.0`，可用仓库变量 `UVCPP_VERSION` 覆盖）
- Win7 x86 那条线还钉在 **1.8.5** 的 mingw32 kit
- 本机开发环境 1.11.0

### 分三步
1. **U1 只升构建**：`UVCPP_VERSION` 1.9.1 → 1.11.0，六个平台的包按官方资产名接入（msvc-x64 / mingw-x64 / linux-x64；Win7 x86 维持 1.8.5 或确认 1.11.0 有 mingw32 包再切）。**不改任何代码**，先把 61 项 ctest 和发布流程跑绿，验证脱敏后的 API 兼容性。
2. **U2 服务层切换**：Windows 用 `uvcpp_service_run` 替换手写服务；安装包改成调用它的 CLI 动词；删掉命名管道与计划任务兜底；同步更新 `service_control.cpp` 与文档。
3. **U3 顺手收编**：`uvcpp_login_limiter` 替换 `lock_state` 的失败计数；评估 `uvcpp_ws_bus`（多 core 部署时才需要）。

### 风险与回退
- 1.9.1 → 1.11.0 跨两个 minor，API 有变动；U1 单独一步就是为了把"编译不过"和"行为变了"分开定位。
- 任何一步红灯：`UVCPP_VERSION` 改回 1.9.1 即回到今天的基线，不影响已发布版本。
- Win7 x86 是唯一可能被卡住的平台（官方 1.11.0 没有 mingw32 包），届时保持 1.8.5 并标注"该平台不随主版本升级"。

## 六、被控端专用版（Windows x86 / x64，极简 D2D）

**定位**：只做被控的安装包。装上就能被远程，没有"去连别人"的能力。给客户/服务器机器用，界面越小越好。

### 6.1 产品形态
- 独立安装包 `moLink Host`（x86 / x64 各一份），与现有全功能版**并存不冲突**（不同 product 名与安装目录）。
- 主窗口是一个**极简 D2D 页面**，只有三块：
  1. **状态**：本机 ID、当前验证码（可复制/刷新）、可达地址与端口、在线/离线
  2. **网络**：监听端口、是否允许远程访问（0.0.0.0 vs 仅本机）、发现开关、中继/直连偏好、IP 白名单
  3. **安全**：单次/多次码、长期密码、两步验证（TOTP）、按功能授权预设（仅查看/仅协助/完整）、访问请求开关
- 底部一行：**服务**（安装/卸载/启动/停止）、**开机自启**（勾选）、日志入口、版本号。
- 没有远程桌面窗口、没有文件面板、没有聊天、没有终端——**这些入口在这个版本里不存在**。

### 6.2 Web 端只能控制"本机"
- 被控端专用版启动的 core 带 `host_only=1`：
  - Web 首页**不显示**"连接设备"表单与最近连接列表
  - 只提供一个「查看本机屏幕」按钮，且只允许连 `127.0.0.1:<port>`（loopback 校验在服务端做，改前端绕不过）
  - 服务端拒绝任何指向**其他设备**的出站会话（`session.outbound_rejected reason=host_only`）
- 其他能力（剪贴板、文件、输入）在"本机自看"这个会话里保持完整，方便管理员就地调试。

### 6.3 生命周期（用户明确要求）
| 场景 | 行为 |
|---|---|
| **已安装为服务** + 关闭窗口 | 窗口关闭，**服务继续跑，仍可被控**；托盘图标保留 |
| **已安装为服务** + 托盘 → 全部退出 | 只退出 UI，服务仍在（明确文案提示"服务仍在后台运行"） |
| **未安装为服务** + 关闭窗口 | **全部退出**，不再被控 |
| 最小化 | 收进托盘（设置里可改为任务栏） |
| 开机自启 | 勾选后写注册表 Run 或计划任务；已装服务时由服务自启，UI 不自启 |

关键实现点：**服务与 UI 是两个进程**，UI 只通过 loopback API 操作 core；服务是否在跑由 SCM 查询，不靠 UI 记账。

### 6.4 兼容性（Win7 必须能用）
- D2D 用 **Direct2D 1.0** + DirectWrite，不碰 1.1 的新接口；Win7 需 Platform Update（安装包检测并提示）。
- 不用 `SetProcessDpiAwarenessContext` 等 8.1+ API，退回 `SetProcessDPIAware`。
- 不依赖 WebView2（这正是做 D2D 而不是嵌网页的原因）。
- x86 包走现有 mingw32 工具链（1.8.5 kit），x64 走 msvc-x64。
- 安装包（Inno Setup）分别出 `moLinkHost-<ver>-x86.exe` / `-x64.exe`，安装目录 `C:\Program Files (x86)\moLink Host` / `C:\Program Files\moLink Host`。

### 6.5 复用与新增
- 复用：`molink-core`（含服务化，见第六节的 1.11.0 升级）、`service_control`、`native_diag`、托盘与自启逻辑（现有全功能壳里已有）。
- 新增：
  - `shell/windows/host_only/`：极简 D2D 应用（状态页 + 设置页 + 服务控制），目标 1500 行以内
  - core 的 `host_only` 模式（服务端出站拒绝 + 可见能力裁剪）
  - Web 前端的 host-only 分支（隐藏连接表单，只留"查看本机"）
  - 安装包脚本与 CI 产物

### 6.6 任务
- [ ] H1 core `host_only` 模式（出站拒绝 + `/api/state` 暴露 `host_only`）+ 单测
- [ ] H2 Web 前端 host-only 分支（隐藏连接、只允许 loopback 自看）+ 前端测试
- [ ] H3 极简 D2D 壳：状态页（ID/验证码/地址/在线）、托盘、关闭语义
- [ ] H4 设置页：网络三项 + 安全四项 + 服务安装/卸载/自启
- [ ] H5 安装包（x86/x64）+ CI 产物 + Win7 实机验证
- **验收**：Win7 x86 装上 → 只显示 ID/验证码 → 另一台机器连进来能看能控 → 关窗口后**仍可被控** → 卸载服务后关窗口即全退 → Web 打开只能看自己，改 URL 也连不到别人。

### 6.7 风险
- 与全功能版并行安装的互斥（端口/服务名/单实例）：服务名用 `moLinkHost`，端口冲突时自动换端口并在 UI 显示实际端口。
- Win7 的 TLS 能力：core 已有静态 OpenSSL，但 Win7 上需确认 TLS 1.2 套件可用（现有全功能版已在 Win7 出包，风险可控）。

## 七、移动端（先 Android，后续 iOS / 鸿蒙）

**定位**：手机**控制别的机器**（viewer）。被控（手机作为宿主）放到下一阶段，且先只做"看屏幕"。

### 7.1 为什么先做 Android
- 同样一套通用框架（Kotlin Multiplatform 或 Flutter）里，Android 的构建与分发成本最低（APK 直接装）
- iOS 需要开发者签名与 TestFlight，鸿蒙需要单独的 SDK，都排在 Android 跑通之后
- 参考 同类产品的移动端交互（触屏手势、悬浮工具条、虚拟键盘/鼠标），但 UI 风格沿用我们自己的（与现有移动 Web 一致）

### 7.2 分两阶段实现

**阶段 A：WebView 壳（能用的 80%）**
- Kotlin 壳 + WebView 加载现有 Web UI，补齐移动端缺失的功能（文件管理、剪贴板、虚拟鼠标/键盘已经在 Web 里有雏形）
- 手势：单指拖动=移动指针，双指=滚动，双指捏合=缩放，长按=右键，三指=呼出工具条
- 系统键盘通过隐藏 input 走 IME 上屏文本（Web 已有 `localIme` 通路）
- 剪贴板：`ClipboardManager` 读写，走现有 `clipboard-sync`
- 文件：`ACTION_OPEN_DOCUMENT` / `ACTION_CREATE_DOCUMENT` + 现有文件通道
- 编码：WebView 的 `VideoDecoder`（Chromium）在较新系统上可用；不可用时退回平铺

**阶段 B：原生渲染 + 硬件解码（低延迟）**
- `MediaCodec` 解码 H.264 / H.265 / VP8 / VP9 / AV1（按设备能力探测，优先 H.264）
- `SurfaceView` 直出，避免 WebView 合成开销
- 与 Web 壳共用会话协议（不新开协议）

**阶段 C（下一阶段，先只做看屏）**
- 被控：`MediaProjection` 抓屏 + 编码上送；输入注入暂缓（需要无障碍服务，风险与审核都高）

### 7.3 编解码与平铺
- 与服务端协商：新增 `android_h264` / `android_vp9` / `android_av1` 能力标志（或复用现有 `h264_stream` 等，由壳注入）
- 服务端编码阶梯：H.264 优先（兼容性最好）→ VP9 → AV1
- 平铺模式保留（低带宽或解码不可用时），与 PC 端同一套 tile 协议
- 目标：1080p@30 在中端机上解码延迟 < 40 ms

### 7.4 外设
- **物理鼠标**：Android 8+ 的 `onGenericMotionEvent` + 指针捕获；右键/滚轮映射
- **物理键盘**：`onKeyDown/onKeyUp` 的 keyCode 映射到我们的 DOM code 表（`keymap.cpp` 已按物理位）
- OTG/USB 与蓝牙都走同一套输入事件

### 7.5 任务
- [ ] M1 Android 壳骨架（Kotlin + WebView + 会话管理 + 权限）
- [ ] M2 触屏手势与虚拟鼠标/键盘
- [ ] M3 文件管理与剪贴板同步（复用 Web 逻辑）
- [ ] M4 原生渲染 + MediaCodec 解码（H.264/VP9/AV1 能力探测）
- [ ] M5 打包与分发（APK，x86_64/arm64）
- [ ] M6（下一阶段）MediaProjection 被控看屏

### 7.6 验收
- 手机连 Windows/Linux 宿主：看画面、触屏操控、虚拟键盘打字、剪贴板双向、传文件
- 物理鼠标键盘（OTG）可用
- 弱网下自动退平铺且不崩
- 1080p 通话级延迟（端到端 < 150 ms 局域网）

### 7.7 风险
- WebView 的 WebCodecs 可用性因系统版本差异大 → 阶段 B 用 MediaCodec 兜底
- 厂商省电策略杀后台 → 前台服务 + 电池优化白名单引导

## 八、Linux 新桌面（Wayland）

**现状**：只有 X11（`x11_capture.cpp` + `x11_input.cpp` + X11 剪贴板）。Wayland 会话下只能看到 XWayland 窗口，原生应用抓不到、也注不进去。

### 8.1 三条通路
| 能力 | Wayland 方案 |
|---|---|
| 屏幕采集 | `xdg-desktop-portal` ScreenCast（PipeWire 流）——**唯一通用方案** |
| 输入注入 | `xdg-desktop-portal` RemoteDesktop（EIS / libei）；`/dev/uinput` 作为无 portal 时的兜底 |
| 剪贴板 | wlroots 系用 `wlr-data-control`；GNOME/KDE 走 portal 或退回"仅 XWayland" |
| 组合键 | EIS 能发 keysym，但**受 compositor 限制**：用户已同意"实现不了就先不做" |

### 8.2 分步
1. **W1 检测与提示**：`XDG_SESSION_TYPE=wayland` 时明确告知"仅 XWayland 应用可同步"，不再静默降级（今天就是这样，用户以为坏了）
2. **W2 采集**：接入 portal ScreenCast（PipeWire），拿到 dmabuf 或内存帧，喂现有编码器
3. **W3 输入**：RemoteDesktop portal（EIS）注入指针与键盘；失败退回 uinput
4. **W4 剪贴板**：`wlr-data-control`（wlroots）优先；GNOME 下走 portal 或标注不支持
5. **W5 组合键**：能发就发，不能就明确禁用并给提示，不做假动作

### 8.3 验收
- GNOME Wayland 下能看到整个桌面（不只 XWayland 窗口），鼠标键盘可用
- KDE Wayland 同上
- 剪贴板在 wlroots 系（Sway/Hyprland）双向可用；GNOME 下如实提示限制
- 组合键：能发则发，不能发时 UI 明确禁用

### 8.4 风险
- portal 需要用户在弹窗里授权，无法静默；每次会话都要选屏幕
- PipeWire 的 dmabuf 路径与编码器零拷贝对接需要额外工作；先做内存帧（性能差一些但正确）

## 九、不做 / 暂缓
- 提权执行（sudo / runas / SYSTEM）：与"以当前用户身份"的约定冲突。
- UDP 端口映射：等 QUIC 数据报通道成型。
- 移动端 Web 的终端键盘适配：先保证桌面浏览器。
