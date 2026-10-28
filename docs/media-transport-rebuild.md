# moLink 媒体传输重构计划

> 状态：进行中
> 更新：2026-10-28
> 当前基线：`60c6412`（在 `cd26f2a` 之上，只有打包与版本号提交）

## 1. 目标

moLink 将远程桌面会话拆成独立的逻辑通道，并为每个通道选择适合的承载方式：

- **视频**：优先 WebRTC 视频轨道；原生端可使用 UDP/QUIC datagram；兼容路径使用 JPEG/AV1/tile over WebSocket。
- **控制**：优先 WebRTC DataChannel 或 QUIC stream；必须可靠、有序、低延迟。
- **文件**：只使用可靠、有背压的 QUIC stream、WebRTC reliable DataChannel、WebSocket 或 TCP。
- **音频**：独立于视频调度，优先实时通道，不能被文件队列阻塞。

WebSocket/TCP 保留为兼容和文件传输路径。视频不会因为线路暂时拥塞而无限排队；过期视频帧应丢弃并通过关键帧反馈恢复。

## 2. 当前代码基线

当前版本已经具备：

- 增量 tile、AV1 patch、AV1 stream 和 JPEG stream。
- AV1 编码时间预算、QP 调整、降采样和客户端 viewer health 反馈。
- libjuice ICE/UDP 直连、UDP relay、QUIC/TCP/WebSocket relay ladder。
- 文件数据在 `switchable_transport` 中倾向可靠 relay。
- 浏览器通过本地 core 的 WebSocket 代理使用 ICE；浏览器还没有直接使用 `RTCPeerConnection`。

当前限制：

- `session_transport` 只有通用 `send_text`/`send_binary`，线路选择依赖包类型和字符串判断。
- 视频仍以 MOLK 包和 pull 请求为主，没有独立的 push scheduler。
- UDP 大消息仍是自定义分片重组，缺少视频帧级过期、选择性恢复和标准 WebRTC 拥塞控制。
- H.264 只有 NVENC 一路（QSV/AMF/VAAPI/VideoToolbox 未接入），浏览器原生 WebRTC 视频轨道未接入。

## 3. 目标架构

```text
capture
  -> video pipeline
       -> H.264 hardware
       -> AV1
       -> JPEG/tile fallback
  -> session channels
       -> control
       -> video
       -> audio
       -> file
  -> transport policy
       -> WebRTC P2P/TURN
       -> WebTransport/HTTP3
       -> native QUIC/UDP
       -> WebSocket/TCP fallback
```

`session_controller` 只负责会话生命周期、鉴权和消息路由；编码、媒体调度和线路选择分别由 video pipeline、channel scheduler、transport policy 负责。

## 4. 开发阶段与验收标准

### Phase 0：基线和通道边界

- [x] 同步并确认最新代码基线。
- [x] 建立 `control/video/audio/file` 逻辑通道标识。
- [x] 所有会话发送路径使用通道 API。
- [x] 为通道选择和发送失败增加单元测试。

验收：旧 transport 实现无需改动即可编译，现有协议测试全部通过。

### Phase 1：通道调度

- [x] 在 transport 上增加通道能力声明。
- [x] 视频、控制、音频、文件使用独立队列和背压预算。
- [x] 视频队列只保留最新帧和必要关键帧。
- [x] 文件发送在 transport 拒绝入队时保留 offset，后续 pull 可重试；新增回归测试。
- [x] scheduler drain 在某通道底层背压时仅暂停该通道，其余通道仍按优先级发送；新增隔离及后续重试测试。
- [x] 调度器压力回归模拟持续文件通道背压：填满有界文件队列、验证控制消息继续发送，并确认释放背压后全部文件消息重试成功。
- [x] 慢接收端下验证真实 transport 文件队列隔离，并完成端到端 relay 压力测试
      （`backend/tests/relay_channel_pressure_test.cpp`：真实 `relay_transport` +
      真实 loopback WebSocket 接收端）。
- [x] 旧接口按 MOLK 包头分类到通道，未迁移调用方（浏览器代理）不再挤进 control 队列。
- [x] `relay_transport` 按在途字节数做背压：libuvcpp 的 ws 客户端本身无界入队且永远返回成功，
      没有这一层调度器的预算在 relay 路径上不生效。

验收：模拟丢包、慢接收端和大文件传输时，视频延迟不会随文件队列线性增长；文件入队失败不会跳过数据，并有自动化测试覆盖。

### Phase 2：视频 push pipeline

- [x] tile 模式保留 pull 兼容（没有在 hello 里声明 `video_push` 的客户端行为完全不变）。
- [x] AV1/JPEG/H.264 模式加入固定发送时钟（`capture/video_push.h` + supervisor 推送线程）。
      实现上不区分解码模式：声明了 `video_push` 的会话在所有模式下都由主机定时发动，
      因为按帧往返在 tile 模式下同样存在；经典拉取路径留给未声明的客户端。
- [x] 编码中最多保留一帧，过期 P 帧直接丢弃：编码在采集线程内串行执行（一次只编一帧），
      推送每次只取 `latest()`，中间帧不会被排进发送队列。
- [x] 关键帧请求、编码时间、发送队列和接收端反馈形成闭环：
      发送失败不再推进 `last_capture_seq`，而是回到“无图像”并请求关键帧；
      viewer 报告的绘制帧率持续低于时钟时时钟减半，恢复后逐级回升；编码超预算的
      QP 自适应沿用原有路径。
- [ ] 真机验收（窗口拖动、滚动、视频播放下的端到端延迟与帧率）尚未做。
- [x] 后台标签页：页面在可见性变化时发 `{"type":"video-push","on":false/true}`，
      隐藏时把会话交还给自己的 1 Hz 拉取，回到前台再交回给主机时钟。
      不开新状态，复用同一个 `video_push` 标志；跳过的帧由序列图和 tile clock 修复。

验收：窗口拖动、滚动和视频播放时，端到端延迟保持有界，显示帧率平稳。

### Phase 3：硬件视频编码

- [x] 统一 `video_encoder` 接口（`capture/video_encoder.h`）：
      `video_codec`（av1_patches / av1_stream / h264_stream）+ `video_encoder` 基类，
      `av1_encoder` 与 `av1_stream_encoder` 都从它派生，未实现的那一半返回 false；
      `video_encoder_backends()` 列出本机后端及其覆盖的等级；`video_ladder()`
      给出观众可解、按优先级排列的等级，capture 依次尝试直到能创建编码器。
      单元测试 `video_encoder_test`。
- [x] NVENC 低延迟流同时支持 **AV1 与 H.264**（`nvenc_stream_encoder`）：
      H.264 用 Baseline/CAVLC、无 B 帧，IDR 才带 SPS/PPS，Annex-B 输出，页面用 WebCodecs 解；
      编码器选择走 `choose_stream_codec()`：全员可解 AV1 → AV1，否则全员可解 H.264 → H.264，
      两者都不齐 → 留在 tile。hello/welcome 新增 `h264_stream` 能力，协议新增 `codec::h264_stream`。
- [x] AV1 低延迟流接入 NVENC（`nvenc_av1_stream_encoder`）：
      `gopLength` 无限、无 B 帧、只在请求时发 IDR，P 帧不重复序列头；
      `fallback_av1_stream_encoder` 在 GPU 连续失败 3 次后整体切到 CPU，
      切换的第一帧强制关键帧（参考链不是 GPU 的），单帧失败则当场用 CPU 重编，
      不丢帧。`device="auto"` 时优先 GPU，主机设置仍可钉死 `cpu-svt`。
      已有 `av1_stream_fallback_test` 覆盖切换逻辑。
- [x] Windows/Linux H.264：NVENC（与 AV1 同一个 `nvenc_stream_encoder`）。
- [ ] H.264 的 QSV / AMF：需要 oneVPL / AMF SDK。Linux 与 210 上都没有头文件，未编写。
- [ ] Linux H.264：VAAPI。构建机没有 `va/va.h`，未编写；接入点是 `make_stream_encoder()`
      的另一个分支，接口不需要再改。
- [ ] macOS H.264：VideoToolbox。没有 macOS 构建环境，未编写。
- [x] AV1/JPEG/tile 作为明确回退等级：`video_ladder()` 排定 AV1 流 → H.264 流 → AV1 patch，
      都不可用时 capture 留在 tile/JPEG；捕捉路径按这条链取第一个能创建的编码器。

验收：编码器切换不改变 session controller 和前端通道协议。

### Phase 4：WebRTC

> 主机侧媒体栈用 **libdatachannel 0.22.6**（自带 libjuice，与原生 P2P 同一套 ICE）。
> 它是可选依赖：`-DMOLINK_ENABLE_WEBRTC=ON` 才编译，默认构建不受影响。

- [x] 增加浏览器 WebRTC 信令和能力协商：hello/welcome 的 `webrtc`、`ice_servers`，
      控制消息 `rtc-offer` / `rtc-answer` / `rtc-ice`，信令始终走原会话 socket。
- [x] 视频使用 WebRTC Video Track：主机是 offerer，声明一条 send-only 的 H.264 轨
      （PT/SSRC 在 SDP 里固定），`H264RtpPacketizer` 直接吃 NVENC 的 Annex-B 输出；
      页面把收到的视频帧画进原来的会话画布，光标、输入映射、全屏都不用改。
- [x] 控制使用可靠 DataChannel：主机创建 `molink` 通道，页面在通道打开后优先用它发控制消息，
      主机侧 inbound 文本走同一条 `on_text` 路径。
- [x] TURN：`MOLINK_TURN_URL` 配置，同时写进 welcome 的 `ice_servers`，两端用同一组服务器。
- [ ] PLI/FIR 与 RTCP 反馈：libdatachannel 自带 PLI/NACK handler，但还没接到
      “收到 PLI 就请求关键帧”的链路上，需要在真实浏览器里调。
- [x] WebSocket 保留兼容回退：没有 `webrtc` 能力、或 offer/answer 失败时，一切照旧走 MOLK。

验收：浏览器能在 P2P、TURN 和 WebSocket 回退之间自动选择，并显示当前路径。

### Phase 5：WebTransport/QUIC

> 主机侧用 libuvcpp **1.9 批次**的 `uvcpp_h3_wt_session`（draft-13）。必须用 1.9 构建
> libuvcpp：1.8.9 没有这个类型。构建时自动探测 `include/http3/uvcpp_h3_wt_session.h`，
> 有就编 `wt_transport`，没有就完全跳过（默认发布用的 1.8.9 不受影响）。

- [x] datagram 承载低延迟数据：`wt_transport` 把 video 通道的帧切成 MTU 大小的
      H3 datagram（8 字节分片头：帧号 / 片号 / 片数），接收端按帧号重组；缺片就整帧丢弃，
      由已有的关键帧/修复回路兜底。超过 256 KiB 的帧（关键帧）改走可靠流。
- [x] stream 承载文件和控制：一条双向流按 MOLK 包头长度分帧，控制/音频/文件都走它，
      与 QUIC 数据流共用 `stream_transport` 的分帧代码。
- [x] 原生端优先 QUIC stream/datagram：原生路径本来就优先 QUIC；WebTransport 是浏览器侧
      的 H3 封装，两者在 `wt_transport` 里共用同一套通道语义。
- [x] 为旧版本客户端保留协议版本协商：WebTransport 是独立入口（h3 上的
      `:protocol = webtransport`），没升级的客户端继续走 WebSocket/QUIC，协议版本不变。
- [x] 浏览器端：`frontend/src/lib/webtransport.ts` 用 `new WebTransport()` 连接，
      证书通过 `serverCertificateHashes` 固定（welcome 里新增
      `webtransport: {port, sha256}`）。可靠流按 MOLK 包头长度分帧收发控制消息，
      datagram 按 8 字节分片头重组视频。页面收到 welcome 后在后台开会话、发 hello，
      拿到 WebTransport 上的 welcome 后关闭原 WebSocket 并把收发切过去；任何一步失败
      都继续用 WebSocket。
- [x] 浏览器能接受的证书：core 另外生成 `tls/webtransport.crt`（ECDSA P-256，13 天），
      剩不到 2 天时自动轮换；它跑在独立端口 **28423**（`kDefaultWebTransportPort`），
      不与 http3/quic 端口冲突。`welcome` 只在监听真的起来时才广播。
- [ ] 浏览器实机验证未做：需要 Chrome 系浏览器（WebTransport + H3 datagram），
      而且本仓没法在这里跑浏览器。
- [ ] **libuvcpp 的服务端 accept 有个 bug**（已定位，patch 见
      `docs/patches/uvcpp-wt-server-open.patch`）：`accept()` 建好会话后既不置 `open_`
      也不调 `on_open`，于是服务端 `open_bidi_stream()` 返回 nullptr、datagram 全被丢弃。
      打上这个 patch 后，`wt_loopback` 端到端通过。

## 5. 当前变更记录

### 2026-10-28

- 已从 GitHub 获取并快进到 `cd26f2a`。
- 新增本重构文档。
- 开始在 `session_transport` 增加逻辑通道边界，保持旧实现兼容。
- 新增 `channel_scheduler`，为四类通道提供独立字节预算、优先级 drain 和视频旧帧淘汰。
- `session_transport` 增加默认能力声明，旧实现继续表示单一可靠路径。
- scheduler drain 的背压隔离已修正：一次 drain 中某通道失败后仅跳过该通道，其余通道按 control、audio、video、file 顺序继续尝试；后续 drain 可重试失败消息。对应测试覆盖“单通道失败其余发送”“全部失败保留消息”和“后续重试成功”。
- 文件队列隔离目前已通过 scheduler 单元测试验证；尚未在真实慢接收端/relay transport 下完成端到端压力测试，因此 Phase 1 仍未验收。
- `switchable_transport` 已按显式通道路由：文件使用仍保持连接的可靠 relay；控制、音频和普通视频使用当前活动线路；关键帧及修复流量保留可靠 relay 路径。旧接口仍按包类型识别，兼容未迁移调用方。
- `switchable` 回归测试验证显式文件/视频/控制路由和旧式包识别；三组通道相关测试均通过。
- 完成 core 全量构建，CTest 13 项中 12 项通过、NVENC 测试因硬件/环境条件跳过。
- 文件下载回调现返回 transport 入队结果；入队失败时文件流回退到未发送 offset。新增 `file_transfer_backpressure` 测试验证重复 pull 会重发同一 offset，入队成功后才推进到下一块。
- Phase 1 尚未完成；待慢接收端下的队列隔离与端到端 relay 压力验证完成后再验收。

### 2026-10-28（接手后第二轮）

- 本轮工作已同步到 `molink` 主工作目录（基线 `60c6412`），全量构建 + CTest 14 项通过
  （`nvenc` 因本机无 GPU 跳过）。
- 修正 `relay_transport` 旧接口的通道映射：原先所有二进制都进 `control` 通道（64 KiB 预算），
  而 `direct_proxy` 的 WebSocket 回退正是用旧接口转发浏览器 256 KiB 上传分块，会被静默丢弃。
  现在按 `protocol::packet_stream_of()` 依包头类型分发，并把关键帧标记传给调度器，
  视频淘汰不会误删关键帧。
- `control` 预算由 64 KiB 提到 256 KiB：剪贴板同步单条上限 192 KiB，原预算会让大段复制静默失败。
- 视频通道允许"单帧超过预算"：无损全屏关键帧可达数十 MiB，拒收等于画面永远出不来；
  此时新来的 P 帧会被丢弃（它们本来也要排在该关键帧之后解码）。
- `relay_transport` 增加在途字节预算（8 MiB）。libuvcpp 的 WebSocket 客户端是**无界入队**且
  永远返回 0，所以调度器 drain 从不阻塞——没有这一层，慢接收端只是把内存搬到客户端的发送队列里。
  现在按写完成回调结算在途字节，超预算时 drain 阻塞该通道，背压才真正传回调度器和文件传输 offset。
  在途字节按压缩前的长度记账，因此对高压缩比流量偏保守（早停而不是晚停）。
- 新增 `relay_channel_pressure_test`：真实 `relay_transport` 连真实 loopback WebSocket 服务端，
  服务端暂停读取后持续压入文件分块。确定性结果：接受 229 条（14.3 MiB）后拒绝，
  恢复接收后 229 条按序、逐字节完整到达，控制消息在文件背压期间仍可入队并送达。
  测试载荷必须不可压缩——uvcpp 默认协商 permessage-deflate，重复字节的 64 KiB 帧会压成几百字节，
  全部塞进内核缓冲，看不出任何背压（第一版测试就栽在这里）。
- Phase 1 完成。下一阶段是 Phase 2（视频 push pipeline）。
- 构建基线升级到 libuvcpp **1.8.9**（`v1.8.9` worktree，Web+WebApp+QUIC+HTTP3，OpenSSL 3.5 静态链接
  进 `libuvcpp.so`，安装到 `~/.local/uvcpp-1.8.9`）。与 1.8.6 一样必须静态链 OpenSSL：
  动态链接时 `libuvcpp.so` 需要 `OPENSSL_3.5.0` 版本的 QUIC 符号，系统 3.0 上直接链接失败。
  升级后全量构建 + 15 项 CTest 通过。
- Phase 2 第一块（主机侧固定发送时钟）：
  - `capture/video_push.h`：纯调度逻辑（节奏、viewer 反馈减半与恢复、上下限），单元测试
    `video_push_test`。
  - hello 新增 `video_push`，welcome 回 `video_push: true`；声明了的会话由 supervisor 的推送线程
    按采集帧率 `post` 到循环里发送，未声明的一律走原拉取路径。
  - 推送 tick 里补上了原先由 `request-frame` 顺带驱动的光标与 `tile_seq`（否则推送会话的光标会冻住、
    丢块的精确修复也会失效）。
  - `send_capture_frame` 发送失败时不再推进会话进度，而是请求关键帧——发送队列进了恢复闭环。
  - 前端：hello 声明 `video_push`，welcome 确认后停止 `request-frame` 轮询。
- 新增 `tools/smoke_video_push.py`：起一个合成画面的 core，用两个真实 WebSocket 会话验证
  “声明推送的会话 3 秒无请求收到约 30 fps”“未声明的会话不主动收帧、拉取仍然可用”。
  本机实测：推送会话 88 帧、拉取会话 0 帧，`SMOKE PASS`。
### 2026-10-28（第三轮）

- 后台标签页回归修复：页面按 `visibilitychange` 发 `video-push` 开关，隐藏时交还给 1 Hz 拉取。
  `tools/smoke_video_push.py` 增加这一段：1.5 s 收到 47 帧 → 关掉后 0 帧 → 再打开 46 帧。
- Phase 3：NVENC 低延迟流编码器，AV1 与 H.264 双编码（见上）。本机没有 GPU，只能做到编译验证；
  运行时行为需要在带 4060 的主机上验证。注意 SVT 分支在本机没有开发头文件，也没有参与编译，
  Windows/发布包里会编译到，改动只在工厂函数。
- 前端新增 `lib/h264-stream.ts`：WebCodecs 解码，按关键帧里的 SPS 现场拼 `avc1.PPCCLL`
  配置串，Annex-B 输入；解码队列落后超过 2 帧就丢 P 帧并请求关键帧。`onFrame` 的位图参数
  放宽为 `ImageBitmap | VideoFrame`，`applyJpegFrameToSession` 更名 `applyStreamFrameToSession`。
  页面只有在 `VideoDecoder.isConfigSupported('avc1.42E01E')` 通过时才在 hello 里声明
  `h264_stream`，所以不支持 WebCodecs 的浏览器行为不变。
- `stream_policy_test` 增加 `choose_stream_codec` 的用例（AV1 优先、H.264 兜底、无共享编码器时留在 tile）。
- Windows（210 / RTX 4060, MSVC x64）实测：后端 17 项 CTest 全过；
  交互会话里 `nvenc_stream_test` 18 项全过（AV1 与 H.264 各一遍）。
  测试暴露出两个 Windows 专有问题，已修：
  1. `file_transfer_backpressure` 链接缺 `ws2_32` + `iphlpapi`（`util.cpp` 的
     `GetAdaptersAddresses`/`inet_ntop`），MSVC 下 LNK2019。
  2. NVENC 回填的 `NV_ENC_LOCK_BITSTREAM.pictureType` 在这台机器上对 P 帧也报 IDR，
     导致每个 delta 都被标成关键帧（接收端会每帧重置解码器）。改为以请求为准。
- SSH 是会话 0，NVENC 一律 `no encode device`；只有交互会话里能用，
  跑法和 `MOLINK_NVENC_LOG=1` 诊断记在 `docs/testing.local.md`。
- 新增 `nvenc_stream_test`：在带 GPU 的机器上对 AV1 / H.264 各跑一遍
  “首帧关键帧 → 三个 delta → 强制关键帧”，校验包非空、关键帧标志正确；无 GPU 时跳过。
  主机记录与构建/测试命令放在 `docs/testing.local.md`（含凭据，已加入 .gitignore）。
- 新增 `av1_stream_fallback_test`（27 项断言）：GPU 正常时不创建 CPU 编码器；GPU 失败时同一帧由
  CPU 重编并强制关键帧；连续 3 次失败后丢弃 GPU；丢弃后不再重试。全量 CTest 16 项通过。
### 2026-10-28（第七轮：Phase 5 浏览器端）

- 页面新增 `lib/webtransport.ts`：`WebTransport` 会话、MOLK 流分帧、datagram 分片重组。
- `client.ts`：welcome 后开 WebTransport 会话并发 hello；收到 WebTransport 上的 welcome
  后关掉原 WebSocket 把收发切过去；失败则完全回退到 WebSocket（`isLive`/`sendJson`/
  `sendRawPacket` 都先看 WT）。
- 主机新增短期证书身份（13 天，提前 2 天轮换）与独立 WebTransport 监听端口 28423；
  welcome 增加 `webtransport: {port, sha256}`。实测：core 起来后 welcome 里带
  `{'port': 28423, 'sha256': '96:d8:...'}`。
- 测试：`build/wt` 20/20、默认构建 19/19、`tsc` + `vite build` 通过。

### 2026-10-28（第六轮：Phase 5 WebTransport）

- 主机侧 WebTransport 传输：`transport/wt_transport`（可靠流 + datagram 分片）与
  `transport/wt_framing`（纯分片/重组逻辑，带单元测试）。
- h3 监听器在 `:protocol = webtransport` 时接受会话，并把对端开的第一条双向流接成可靠路径；
  datagram 只走视频通道。
- CMake：探测 libuvcpp 是否带 WT 头文件，有则编入并定义 `MOLINK_HAVE_WEBTRANSPORT=1`；
  默认（1.8.9）构建完全不受影响。
- 测试：`wt_framing`（分片形状、乱序重组、丢片、旧帧被新帧顶掉、畸形输入）与
  `wt_loopback`（真实 QUIC 上的 WebTransport 会话：控制包双向、3 片视频帧重组）。
  在 libuvcpp 1.9 + 服务端 patch 下 20/20 通过。
- 发现并记录 libuvcpp 的 `accept()` 缺陷（patch 存在 `docs/patches/`，没有改上游工作树）。

### 2026-10-28（第五轮：Phase 4 WebRTC）

- 可选依赖 libdatachannel 0.22.6（`MOLINK_ENABLE_WEBRTC=ON`，默认关闭）。
- `webrtc/rtc_session`：主机作为 offerer，一条 H.264 send-only 视频轨 + 一个 `molink`
  数据通道；信令（offer/answer/candidate）通过原有控制通道传递，`MOLINK_STUN_HOST` /
  `MOLINK_TURN_URL` 同时用于主机和页面。
- 视频帧在 `send_frame_with_av1` 里分流：H.264 流且有打开的 WebRTC 轨时走 RTP，
  不再发 MOLK 帧；页面收到的轨道被画进原来的会话画布，渲染/输入/光标路径不变。
- 数据通道：页面在通道打开后优先用它发控制消息；主机 inbound 走同一个 `on_text`。
- `webrtc_loopback_test`：同进程两端的完整握手 —— ICE 连接、数据通道双向文本与二进制、
  H.264 访问单元经 RTP 打包/解包后字节一致。19 项 CTest（WebRTC 构建）全过。
- `tools/smoke_webrtc_offer.py`：真实 core 上验证 welcome 能力位与 offer 内容
  （828 字节 SDP，含 video / application / H264），PASS。
- 未做：PLI→关键帧的回链路（需真浏览器）。

### 2026-10-28（第四轮：Phase 3 收口）

- 统一编码器接口：`capture/video_encoder.h` 定义 `video_codec`、`video_encoder` 基类、
  后端能力表 `video_encoder_backends()` 与等级链 `video_ladder()`；
  `av1_encoder` / `av1_stream_encoder` 改为派生自基类，NVENC 两套实现都标记 `hardware()`。
- capture 不再写死“AV1 否则 H.264”的分支，而是按 `video_ladder()` 依次尝试创建编码器，
  观众能力与主机后端能力在同一个地方汇合。
- `stream_codec` 更名为 `video_codec`（取值 av1_patches / av1_stream / h264_stream），
  避免“只有流才有编码器”的误导。
- 新增 `video_encoder_test`（15 项断言）：等级顺序、后端表自洽、命名、基类默认值。本机 18 项 CTest 全过。
- Windows 复测：同一套改动在 210 上重新构建并通过 CTest。
- Phase 4/5 的阻塞点写进计划正文。

- 边界说明：relay 路径上四条通道仍共用一条 TCP 流，所以隔离是**队列层面**的——
  控制/视频最多排在约 12 MiB（4 MiB 队列 + 8 MiB 在途）文件数据之后，而不是无界增长。
  真正的按通道物理隔离要等 Phase 4/5 的 WebRTC / WebTransport。

## 6. 设计约束

1. 不修改 `MOLK` magic、现有 packet header 或旧客户端可用的 WebSocket 行为。
2. 新通道 API 默认适配旧 transport，分阶段替换线路实现。
3. 文件必须有明确的可靠性和背压边界；视频不能复用文件的无限等待语义。
4. 不把“ICE/UDP”称为浏览器原生 WebRTC。浏览器 WebRTC 接入前，当前浏览器路径仍是 WebSocket 到本机 core 的代理。
5. 每个阶段都必须有纯单元测试或可重复的 loopback/压力测试，不能只依赖人工观感。
