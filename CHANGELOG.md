# Changelog

## [0.1.0] — Unreleased

### Added

**molink-core（C++20 / libuvcpp）**
- HTTP/WS 后台：`/healthz`、`/api/health`、`/api/info`、`/api/sessions`、`/ws/session`
- 令牌鉴权（Bearer / `X-MoLink-Token`）、默认仅回环地址、CORS 白名单
- MOLK 协议 v1：32 字节包头、瓦片帧体、关键帧/增量帧、有界帧历史与补发
- Linux X11 抓屏：XShm 全量抓取、XDamage 受损矩形、瓦片 diff、30 秒全量刷新
- 无会话时暂停抓屏；会话断开释放按键与鼠标按钮
- 瓦片 zlib 压缩（codec 5 `deflate_bgra`），不划算时逐帧回退原始 BGRA
- XTEST 输入注入：指针、按键、滚轮、组合键
- XFixes 光标位图与位置（规格化坐标 + 热点）
- X11 文本剪贴板双向同步（UTF8_STRING，支持 INCR 读取）
- 父进程监视（`--parent-pid`）、`--print-ready` 就绪行
- `--no-capture` / `--no-input` / `--no-compress` / `--no-clipboard` 开关

**frontend（React 19 + TypeScript）**
- 连接页 / 会话页，中英双语，深色主题
- MOLK 解码、BGRA 与 deflate 瓦片渲染、帧序保证
- 控制模式（手动开启）、归一化坐标映射、组合键与滚轮
- 远端光标覆盖层（控制模式下由本地光标取代）
- 剪贴板复制/发送按钮
- FPS / 延迟 / 带宽 / 帧数统计

**shell / 打包 / 仓库**
- C++20 + GTK3 + WebKitGTK 桌面壳：启动核心、解析 ready 行、加载核心托管的前端、
  退出回收、F11 全屏、`--smoke-test`
- 核心静态托管前端（`--web-root`），Web 版与桌面壳共用同一入口
- `.deb` 打包：`/opt/molink/{bin,lib,web}`，核心 `$ORIGIN/../lib` 查找随包库
- CI（前端 lint/typecheck/build、协议测试、壳层编译）
- Linux 发布工作流（libuvcpp 源码构建、deb 产物）
- MIT LICENSE、协议文档、发布检查清单、上游问题记录

### Verified

- 36 项协议编解码断言
- Xephyr 嵌套 X server 中的输入注入、光标与剪贴板端到端验证
- 压缩路径像素级比对（画布中心像素与源窗口一致）
- 1280×800 / 2880×1920 真实桌面抓屏；活跃会话约 7–24% 单核、7.4 Mbps（zlib）
- 桌面壳在 Xephyr 中通过冒烟测试（`SHELL_SMOKE_OK title=moLink`）
- 解包 `.deb` 后从暂存目录启动壳层，冒烟测试同样通过
- `uvp_verify`：健康检查、鉴权、根路由断言全部通过

### Known gaps

- Windows / macOS 的抓屏与输入注入未实现
- Wayland / PipeWire 门户抓屏未实现（没有 dev 头文件，无法在本机构建验证）
- H.264 / AV1 + WebCodecs 未接入（本机无编码器库）
- UDP 直连、WebRTC、P2P、`moLink Server`、移动端属于二阶段
- Windows / macOS 壳未实现（需要 WebView2 / WKWebView）
- AppImage 尚未生成（当前产物为 `.deb`）
