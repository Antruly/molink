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

**shell / 仓库**
- Tauri 2 壳层源码：启动并监管核心、注入端口与令牌、退出回收
- CI（前端 lint/typecheck/build、协议测试、shell check）
- 桌面发布工作流（三平台矩阵，libuvcpp 源码构建）
- MIT LICENSE、协议文档、上游问题记录

### Verified

- 36 项协议编解码断言
- Xephyr 嵌套 X server 中的输入注入、光标与剪贴板端到端验证
- 压缩路径像素级比对（画布中心像素与源窗口一致）
- 1280×800 / 2880×1920 真实桌面抓屏；活跃会话约 7–24% 单核、7.4 Mbps（zlib）
- `uvp_verify`：健康检查、鉴权、根路由断言全部通过

### Known gaps

- Windows / macOS 的抓屏与输入注入未实现
- Wayland / PipeWire 门户抓屏未实现（没有 dev 头文件，无法在本机构建验证）
- H.264 / AV1 + WebCodecs 未接入（本机无编码器库）
- UDP 直连、WebRTC、P2P、`moLink Server`、移动端属于二阶段
- Tauri 壳层未编译：本机缺 Rust 工具链与 `webkit2gtk-4.1` 开发包
- 未生成任何安装包
