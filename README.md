# moLink

跨平台远程桌面。Web 优先，一套后台 + 浏览器壳层，桌面端直连，移动端经服务端中转。

> 版本 0.2.0 · MIT · 协议版本 1

![主页（亮色）](docs/screenshots/home-light.png)
![会话页（合成测试帧）](docs/screenshots/session-light.png)

> 会话页截图为合成测试帧演示；接入真实屏幕后画面即为对端桌面。暗色主题见
> [home-dark.png](docs/screenshots/home-dark.png)。

## 当前状态

一阶段（桌面直连）已完整实现：Linux（X11）、Windows（DXGI）、macOS（CoreGraphics）各有抓屏、输入注入、光标与剪贴板后端，三个平台使用系统 WebView 的独立壳层。界面采用“设备页 + 多会话”布局，支持亮/暗主题、无边框圆角窗口、托盘与连接质量水印。

| 能力 | 状态 |
|---|---|
| `molink-core` HTTP/WS 后台、令牌鉴权、回环限制 | ✅ |
| MOLK 瓦片帧包（C++ + TS）、增量帧、关键帧恢复 | ✅ 36 项协议测试 |
| Linux 抓屏/输入/光标/剪贴板 | ✅ X11 + XShm + XDamage · XTEST · XFixes |
| Windows 抓屏/输入/剪贴板 | ✅ DXGI Desktop Duplication · SendInput · CF_UNICODETEXT |
| macOS 抓屏/输入/剪贴板 | ✅ CoreGraphics · CGEvent · NSPasteboard |
| 设备 ID / 验证码（持久化，默认隐藏） | ✅ 令牌保存在配置目录 |
| 首次连接独立窗口、后续连接 Tab、标签拖出 | ✅ 三个平台的原生多窗口 |
| 亮/暗主题、无边框圆角、自绘标题栏、关闭到托盘 | ✅ Linux/Windows；macOS 使用原生标题栏 |
| 连接质量水印（默认关闭） | ✅ |
| 瓦片 zlib 压缩（codec 5） | ✅ 真实桌面实测 94 → 7.4 Mbps |
| Linux Wayland / PipeWire | ⏳ 回退合成帧 |
| H.264 / AV1 + WebCodecs | ⏳ |
| UDP 直连、WebRTC、P2P、服务端、移动端 | ⏳ 二阶段 |

## 架构

```
┌──────────────┐        ┌────────────────────────────────┐
│ 桌面壳层     │        │ molink-core (C++/libuvcpp)     │
│ React 前端   │◄──────►│ HTTP/WS 127.0.0.1:21118        │
└──────────────┘        │ 捕获/编码 · 输入注入 · UDP     │
                        └────────────────────────────────┘
```

- **molink-core**：C++20 + libuvcpp 后台。负责 HTTP/WS 接口、三平台捕获/注入/剪贴板、瓦片帧编码与会话管理。
- **frontend/**：React 19 + TypeScript + Vite + Tailwind 4 + Zustand + react-i18next。
- **shell/**：三个平台各自的原生壳层（Linux GTK/WebKitGTK、Windows WebView2、macOS WKWebView），启动并监管 `molink-core`，加载核心托管的前端。
- **server/**、移动端：二阶段。

## 目录

```
molink/
├── .github/workflows/   ci.yml · release-desktop.yml · release-mobile.yml（占位）
├── backend/             molink-core（CMake，libuvcpp）
├── frontend/            React 前端
├── shell/native/        Linux 壳层（GTK3 + WebKitGTK）
├── shell/windows/       Windows 壳层（Win32 + WebView2）
├── shell/macos/         macOS 壳层（Cocoa + WKWebView）
├── packaging/           linux(.deb/.AppImage/.rpm) · windows(NSIS/portable) · macos(.dmg)
├── docs/
│   ├── dev.md           架构基线与发布策略（需求文档）
│   └── protocol.md      MOLK 线协议与 HTTP/WS 契约
├── LICENSE
└── README.md
```

## 快速开始

前置：CMake ≥ 3.20、C++20 编译器、libuvcpp ≥ 1.7（编译时开启 `UVCPP_BUILD_WEB` 与 `UVCPP_BUILD_WEBAPP`）、Node.js LTS + pnpm。

```bash
# 后台
cmake -S backend -B backend/build -DCMAKE_BUILD_TYPE=Release
cmake --build backend/build
MOLINK_TOKEN=dev-token ./backend/build/molink-core --print-ready

# 前端（另一个终端）
cd frontend && pnpm install && pnpm dev
```

Linux 默认使用 X11/XShm 抓取当前屏幕；在 Wayland 会话或没有显示器的环境里会自动回退到合成测试帧。可显式控制：

```bash
./backend/build/molink-core --capture-fps 15      # 抓屏帧率上限
./backend/build/molink-core --no-capture          # 强制合成测试帧
./backend/build/molink-core --no-input            # 禁止指针/键盘注入
./backend/build/molink-core --no-compress         # 关闭 zlib 瓦片压缩
./backend/build/molink-core --no-clipboard        # 关闭剪贴板同步
MOLINK_DISPLAY=:1 ./backend/build/molink-core     # 指定 X display
```

抓屏只在有已鉴权会话时运行；没有客户端时进程空闲占用接近 0%。

会话页需要点击「开始控制」后才会把指针与键盘事件发送到远端，默认不接管输入。

打开 <http://localhost:1420>，填入令牌连接。也可以直接带参数自动连接：

```
http://localhost:1420/?host=127.0.0.1&port=21118&token=dev-token
```

桌面壳层（Linux，需要 `libgtk-3-dev` 与 `libwebkit2gtk-4.1-dev`）：

```bash
cmake -S shell/native -B shell/native/build -DCMAKE_BUILD_TYPE=Release
cmake --build shell/native/build --parallel
./shell/native/build/moLink
```

生成 Linux 安装包：

```bash
./packaging/linux/build-deb.sh       # build/package/molink_0.2.0_amd64.deb
./packaging/linux/build-appimage.sh  # build/package/moLink-0.2.0-x86_64.AppImage
./packaging/linux/build-rpm.sh       # build/package/molink-0.2.0-1.x86_64.rpm
```

Windows（需 WebView2 SDK 与 NSIS）与 macOS（`.app` + `.dmg`）的构建步骤见
`packaging/windows/` 与 `packaging/macos/`，发布工作流会在对应 runner 上自动完成。

> 本机若出现 `libllhttp.so.9.2: cannot open shared object file`，说明安装的
> `libuvcpp.so` 缺少 `RUNPATH`。本项目的 CMake 已用旧式 `RPATH` 让间接依赖可解析；
> 发布包仍需把 `libuvcpp`、`libllhttp` 等共享库一并打入。

## 界面

- **设备页**：展示本机设备 ID 与验证码（默认以 `•` 隐藏，可显示/复制），以及抓屏、输入、剪贴板能力状态。
- **连接**：输入对方地址、端口与验证码。首次连接在独立窗口打开，之后的连接进入 Tab；标签可拖出成独立窗口。
- **窗口**：Linux/Windows 为无边框圆角窗口 + 自绘标题栏，关闭按钮收进托盘；macOS 使用原生标题栏，关闭后保留在 Dock。
- **主题**：亮色 / 暗色一键切换，跟随系统初始值并记住选择。
- **质量水印**：默认关闭，可在偏好设置或会话工具栏开启，显示帧率、延迟与带宽。

## 测试

```bash
# 协议编解码（不依赖 libuvcpp，CI 也跑这一组）
cmake -S backend -B backend/build -DMOLINK_BUILD_CORE=OFF && cmake --build backend/build
ctest --test-dir backend/build --output-on-failure

# 前端
cd frontend && pnpm lint && pnpm typecheck && pnpm build
```

## 协议

见 [docs/protocol.md](docs/protocol.md)。要点：

- 所有传输共用同一套 **MOLK** 帧包；Magic `"MOLK"` 不可更改。
- 包头 32 字节，帧体 16 字节 + 16 字节/瓦片 + 瓦片字节，全部小端。
- WebSocket 通道上，二进制消息 = 一条完整 MOLK 包，文本消息 = JSON 控制消息。
- 鉴权位于应用层：连接后必须发送 `hello`，令牌错误以 1008 关闭。

## 发布

推送 `v*` 标签触发 GitHub Actions，三个平台并行构建并附加到同一个 Release：

- Linux：`.deb`、`.AppImage`、`.rpm`
- Windows：NSIS 安装器 `.exe` + 免安装 `.exe`
- macOS：Apple Silicon 与 Intel 的 `.dmg`

桌面端不发布 zip；服务端与移动端资产属于二阶段。

## 发布状态

版本记录见 [CHANGELOG.md](CHANGELOG.md)，发布前检查项与当前阻塞见
[docs/release-checklist.md](docs/release-checklist.md)。

| 平台 | 产物 | 状态 |
|---|---|---|
| Linux | `.deb`、`.AppImage`、`.rpm` | 本机验证；RPM 由 CI 构建 |
| Windows | 安装器 `.exe`、免安装 `.exe` | 壳层交叉编译通过，由 CI 打包 |
| macOS | `.dmg` | 由 macOS CI 构建 |

AppImage 首次使用需要 `chmod +x`，并依赖系统的 GTK 3 与 WebKitGTK 4.1；缺少依赖时 AppRun 会弹出说明。

推送 `v*` 标签会触发 `.github/workflows/release-desktop.yml`，三个平台并行构建并把
产物附加到同一个 Release；需要仓库变量 `UVCPP_REPO`（libuvcpp 源码地址）。

## 许可证

MIT。
