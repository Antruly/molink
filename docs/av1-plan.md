# AV1 自适应编码计划

目标：小改动继续走现有瓦片压缩（无损/近无损），短时间大面积变化的帧走 AV1；
AV1 只编码合并后的变化矩形，而不是每个瓦片单独编码或整屏编码。实时性优先。

## 决策（2026-10-26 与维护者确认）

- 文字/UI 区域继续用现有 deflate/rgb565 瓦片，保证清晰度。
- 视频/游戏类高变化区域用 AV1；第一版每个矩形独立 **intra** 流，不维护参考帧；
  全帧 inter / 长期参考作为后续优化。
- **无损档不走 AV1**：超清/无损/平衡/raw 档的 `quality_profile.lossless` 为
  true，必须保持 BGRA 逐像素一致；AV1 只在流畅/游戏/极速等有损档启用。当前
  SVT-AV1 是 4:2:0，QP 0 也不是逐像素无损（实测 128×128 luma 有
  1773/16384 个像素不同），发行包还只支持 4:2:0。以后要在无损档用 AV1，
  需要 4:4:4 + lossless 的编码器（如 libaom）并接受 CPU/延迟代价。
- 相邻脏瓦片先合并成矩形（每帧限制数量与总面积），再交给编码器。
- 编码器抽象 `av1_encoder`：先接 **SVT-AV1**（BSD-3，CPU）；后续可接
  NVENC（Windows）、VideoToolbox（macOS）。
- 解码：
  - 本机 core 用 **dav1d** 解码后把原始矩形交给页面（兼容性最好）；
  - web 端在 `VideoDecoder.isConfigSupported` 支持时用 WebCodecs 解码；
  - 不支持就回退瓦片压缩。
- 能力协商：客户端在 `hello` 里声明 `av1: true/false`；主机只在支持时发 AV1。
  Win7 x86 客户端默认 `av1=false`，主机继续用瓦片。
- 色度：第一版 AV1 用 4:2:0；若 WebCodecs/目标浏览器实测支持 profile 1（4:4:4），
  再对视频区域开 4:4:4。文字区域永远走无损瓦片，不受色度影响。

## 协议

现有 `codec::av1` 已经存在，`tile.data` 直接放 AV1 码流：

- 每个 AV1 瓦片 = 一个合并后的矩形，`x/y/w/h` 标明落点；
- 第一版每个矩形是一段独立的 intra 码流，可单独解码；
- 页面的解码结果按 `x/y` 绘制；
- 帧头 `keyframe` 仍表示整屏基准；AV1 矩形随普通帧发送。

后续如果要 inter 预测，需要为每个矩形维护参考帧 id，并保证解码顺序；
这与当前“瓦片独立”的模型不同，单独设计。

## 编码流水线

1. 捕获线程完成 tile diff，得到脏瓦片集合；
2. `merge_dirty_rects()` 把相邻瓦片合并成矩形（行/列优先，限制数量/面积）；
3. 小矩形/文字区域：现有 `encode_tile`；
4. 大矩形且高变化：`av1_encoder->encode_rect(pixels, rect)`；
5. 编码结果进入 FRAME payload，附带 codec=av1 的 tile；
6. 编码器实例按 (宽,高,preset) 复用，避免每帧初始化。

SVT-AV1 初版参数：low-delay、preset 10–13、CBR 或 QP、8-bit 4:2:0；
按矩形尺寸创建/复用编码器；编码线程与捕获线程分离，帧率不够时丢弃旧帧。

## 解码流水线

- 本机 core：dav1d 解码每个矩形，输出 BGRA/RGB，按现有 tile 路径发给页面；
- web：`VideoDecoder`（codec `av01.0.04M.08`），每个矩形一个 decoder 或按尺寸池化；
  解码异步，按 `x/y` 绘制；失败/不支持时请求瓦片重发或退回瓦片模式。

## 能力与回退

- `hello` 增加 `av1` 能力；`welcome` 返回 `tiles.av1` 表示主机可用；
- 客户端平台/浏览器不支持时 `av1=false`；
- Win7 x86 构建不链接 SVT-AV1，主机对 Win7 客户端始终发瓦片；
- 编码失败/过慢自动退回瓦片，不影响实时。

## 依赖与打包

- Linux：`libsvtav1-dev` / 静态库 + `libdav1d-dev`（dav1d 用于解码）；
- Windows：SVT-AV1 x64 预编译 DLL/静态库；Win7 x86 构建排除；
- macOS：SVT-AV1 + dav1d（VideoToolbox 作为后续硬件路径）；
- 许可证说明加入 `docs/third-party-notices.md`；
- CMake 可选依赖：没有 SVT-AV1 时 `MOLINK_HAVE_SVT_AV1=0`，构建继续，功能回退。

## 测试

- 合并矩形单测：随机脏瓦片集合 → 矩形覆盖完整且不重复；
- 编解码往返：静态 UI、文字、滚动、视频、游戏各 30 秒，比较画质/延迟/CPU；
- 与瓦片压缩的带宽/延迟对比（同一场景、同一帧率）；
- Win10 x64 主机 + Win10/浏览器客户端；Win7 x86 客户端确认回退瓦片；
- 断线/关键帧恢复：AV1 矩形与现有 keyframe 逻辑共存。

## 进度（2026-10-26）

- 已完成：`merge_dirty_tiles()`（脏瓦片合并、数量上限、覆盖/不重叠单测）、
  `av1_encoder` 抽象与 SVT-AV1 实现（4:2:0 full-range BT.709、intra keyframe、
  按尺寸复用）、dav1d 往返测试。默认构建不启用，不影响现有瓦片路径。
- SVT-AV1 的低延迟流水线会延迟一帧输出，单帧调用 `pic_send_done=1` 也不会
  flush；实现里对同一矩形发送两次并丢弃上一次留下的重复帧，测试覆盖复用与
  改尺寸。
- 已接入（0.9.22）：捕获端把 ≥2 瓦片的大矩形拆成 AV1 补丁，作为 tile-refresh
  随基础帧发送（仅限有损档，且所有已认证会话都支持 AV1 时）；连接代理在 hello
  里声明 av1；客户端 core 在工作线程用 dav1d 解码后按 BGRA 交给页面；AV1 补丁
  和关键帧走中继可靠路径。
- 2026-10-26 实测：1280×720 全屏动画，AV1 补丁每帧约 48 KB，客户端解码成
  3.6 MB BGRA，10+ fps；主机和客户端两端都需要 0.9.22。
- 2026-10-26 实测 WebCodecs：同一段 AV1 码流 Chrome 能解（128×128），
  WebKitGTK 2.50/2.52 的 `VideoDecoder` 全部返回 Decode error（各种 level、
  av1C description、硬/软解选项都试过），所以原生壳层不能依赖 WebCodecs，
  客户端 core 必须带 dav1d 解码；浏览器端以后可以再加 WebCodecs 快路。

## 当前环境记录

- Linux 运行库已有 `libSvtAv1Enc.so.1`、`libdav1d.so.7`；开发头文件未安装，
  但不装 root 也能取到：
  `apt-get download libsvtav1-dev libdav1d-dev`，`dpkg-deb -x` 到
  `/tmp/molink-av1-sdk/root`，再给 `usr/lib/x86_64-linux-gnu` 建指向系统
  `libSvtAv1Enc.so.1` / `libdav1d.so.7` 的 `.so` 符号链接即可。
- 构建：`cmake -S backend -B backend/build -DMOLINK_ENABLE_AV1=ON
  -DMOLINK_AV1_SDK=/tmp/molink-av1-sdk/root`；`ctest` 会多跑
  `dirty_rects` 和 `av1_roundtrip` 两个测试。默认（`MOLINK_ENABLE_AV1=OFF`）
  不链接任何 AV1 库。
- 本机 Chrome 155 支持 WebCodecs；在 headless `about:blank` 里 `VideoDecoder`
  未暴露，需要用 127.0.0.1 页面/真实窗口再确认 4:2:0/4:4:4 profile 支持。
- Win7 x86 构建继续不链接 AV1，客户端回退瓦片。

## 实施顺序

1. `av1_encoder` 接口 + 合并矩形 + CMake 可选依赖（先无编码器，纯结构和测试）；
2. SVT-AV1 编码器（Linux/Windows x64）+ 能力协商；
3. 本机 core dav1d 解码 + 页面绘制；
4. WebCodecs 解码（支持时）+ 回退；
5. 画质/延迟调优，preset 与矩形阈值；
6. 硬件编码（NVENC/VideoToolbox）作为后续。
