# Changelog

## [0.3.0] — 2026-10-21

### Added

- 本机（回环）访问免验证：页面通过 `/api/device` 自动取得本机令牌
- 非本机访问核心 UI 必须输入验证码；验证后可进入主页并操作该设备
- 远程验证后可一键“控制这台设备”（核心所在主机），无需再次验证
- 空闲自动锁定：默认 60 秒，可 `--lock-timeout` 或偏好设置修改并持久化
- 锁定仅在“无远程会话且无操作”时触发；多窗口会话数通过 `/api/ui-state` 汇总
- `/api/state`、`/api/login`、`/api/activity`、`/api/ui-state`、`/api/lock-timeout`
- 登录失败限速（5 次失败后暂停 30 秒）
- 本机自连防护：设备页隐藏“连接本机”，表单拒绝回环地址 + 本机端口

### Changed

- 壳层 URL 不再携带验证码；本机令牌由回环 `/api/device` 获取
- 远程模式下新连接使用 Tab（避免独立窗口二次登录）；标签拖出仍限本机
- 锁定状态下 WebSocket 握手被拒绝（`1008 locked`），HTTP API 返回 `423`

### Fixed

- 设备页锁定时长下拉框对非预设值显示错误（现在动态包含当前值）

## [0.2.0] — 2026-10-21

### Added

**界面**
- 设备页：本机设备 ID 与验证码（默认隐藏、可复制），能力状态，最近连接
- 多会话：首次连接在独立窗口打开，后续连接进入 Tab；标签可拖出成独立窗口
- 亮色 / 暗色主题，跟随系统初始值并记住选择
- 连接质量水印，默认关闭，可在偏好设置或会话工具栏开启
- 无边框圆角窗口 + 自绘标题栏（Linux/Windows），边缘拖拽缩放
- 关闭窗口收进托盘（Linux GtkStatusIcon / Windows Shell_NotifyIcon）；macOS 保留在 Dock
- 新应用图标：青绿→天蓝渐变 + 白色 “m”，同时用于窗口、托盘与安装包

**后台**
- 持久化设备身份：设备 ID 与验证码保存在配置目录，验证码不再每次启动变化
- `/api/device`（仅回环地址）向本机界面提供设备信息
- 修复系统分配端口时 `/api/device` 与 `/api/info` 报告 `port=0` 的问题

**壳层**
- 统一 `--url` 模式：独立会话窗口复用同一个壳层进程，无需再启动核心
- Linux：WebKit `create` 信号派生新窗口；GTK 窗口消息处理
- Windows：WebView2 `NewWindowRequested`、WebMessageReceived、托盘、圆角区域
- macOS：`WKUIDelegate` 新窗口、脚本消息、关闭隐藏、`--url` 模式

**打包**
- Windows 免安装单文件 `.exe`（NSIS 静默解压到临时目录后运行）
- Linux RPM（`rpmbuild`，与 `.deb` 相同布局）
- macOS `.app` 使用 `icon.icns`，dmg 包含应用图标
- AppImage 启动前检查缺失依赖并给出可读提示；补齐 `.DirIcon`

### Fixed
- 标签页弹窗重复、首次连接与后续连接的窗口/Tab 规则
- 无边框窗口无法拖动/缩放（原生消息 + 边缘热区）

### Known gaps
- Wayland/PipeWire 尚未支持，会回退合成帧
- H.264/AV1 未接入
- Windows/macOS 后端由 CI 编译，真机回归仍待进行
- Windows/macOS 安装包未签名；macOS 为 ad-hoc 签名

## [0.1.0] — 2026-10-21

### Added

**molink-core（C++20 / libuvcpp）**
- HTTP/WS 后台：`/healthz`、`/api/health`、`/api/info`、`/api/sessions`、`/ws/session`
- 令牌鉴权（Bearer / `X-MoLink-Token`）、默认仅回环地址、CORS 白名单
- MOLK 协议 v1：32 字节包头、瓦片帧体、关键帧/增量帧、有界帧历史与补发
- Linux X11 抓屏：XShm 全量抓取、XDamage 受损矩形、瓦片 diff、30 秒全量刷新
- Windows 抓屏：DXGI Desktop Duplication、瓦片 diff、光标位图（图标遮罩合成）
- macOS 抓屏：CoreGraphics 主屏抓取、瓦片 diff、屏幕录制权限提示
- 无会话时暂停抓屏；会话断开释放按键与鼠标按钮
- 瓦片 zlib 压缩（codec 5 `deflate_bgra`），不划算时逐帧回退原始 BGRA
- 输入注入：Linux XTEST、Windows SendInput、macOS CGEvent；指针、按键、滚轮、组合键
- XFixes 光标位图与位置（规格化坐标 + 热点）
- 文本剪贴板双向同步：Linux X11 selection（INCR）、Windows CF_UNICODETEXT、macOS NSPasteboard
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
- Linux 壳：GTK3 + WebKitGTK，启动核心、解析 ready 行、加载核心托管的前端、
  退出回收、F11 全屏、`--smoke-test`
- Windows 壳：Win32 + WebView2，运行时加载 WebView2Loader.dll，同样的
  核心监管与 `--smoke-test`
- macOS 壳：Cocoa + WKWebView，.app 布局（Contents/MacOS + Resources/web）
- 核心静态托管前端（`--web-root`），Web 版与桌面壳共用同一入口
- Linux `.deb`：`/opt/molink/{bin,lib,web}`，核心 `$ORIGIN/../lib`
- Linux `.AppImage`：AppDir 布局，依赖系统 WebKitGTK
- Windows NSIS 安装器、macOS `.dmg` 打包脚本
- CI：前端、协议、三个平台壳层编译
- 发布工作流：Linux/Windows/macOS 并行构建并附加到同一 Release
- MIT LICENSE、协议文档、发布检查清单、上游问题记录

### Verified

- 36 项协议编解码断言
- Xephyr 嵌套 X server 中的输入注入、光标与剪贴板端到端验证
- 压缩路径像素级比对（画布中心像素与源窗口一致）
- 1280×800 / 2880×1920 真实桌面抓屏；活跃会话约 7–24% 单核、7.4 Mbps（zlib）
- Linux 桌面壳在 Xephyr 中通过冒烟测试（`SHELL_SMOKE_OK title=moLink`）
- 解包 `.deb`、运行 `.AppImage` 后冒烟测试同样通过
- Windows 壳用 MinGW-w64 交叉编译通过（链接成功并产出 WebView2Loader.dll）
- `uvp_verify`：健康检查、鉴权、根路由断言全部通过

### Known gaps

- Windows 与 macOS 后端尚未在真实平台运行验证（Windows 通过 MinGW 编译，macOS 待 CI 编译）
- macOS 未提供远端光标位图（仅本地光标），且需要屏幕录制与辅助功能权限
- macOS 包为 ad-hoc 签名，正式分发需要 Developer ID 与公证
- Wayland / PipeWire 门户抓屏未实现
- H.264 / AV1 + WebCodecs 未接入
- UDP 直连、WebRTC、P2P、`moLink Server`、移动端属于二阶段
