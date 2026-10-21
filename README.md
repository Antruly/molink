# moLink

跨平台远程桌面。Web 优先，一套后台 + 浏览器壳层，桌面端直连，移动端经服务端中转。

> 版本 0.1.0 · MIT · 协议版本 1

![连接页](docs/screenshots/connect.png)
![会话页](docs/screenshots/session.png)

> 会话页截图为合成测试帧演示；接入真实屏幕后画面即为本机桌面。

## 当前状态

一阶段（桌面直连）的骨架已经可运行。Linux（X11）已接入真实屏幕捕获；Windows / macOS 的捕获后端与输入注入尚未实现。

| 能力 | 状态 |
|---|---|
| `molink-core` HTTP/WS 后台 | ✅ 可构建、可运行 |
| 令牌鉴权、回环地址约束、CORS | ✅ |
| MOLK 瓦片帧包编解码（C++ + TS 双实现） | ✅ 36 项协议测试通过 |
| WebSocket 会话通道、心跳、空闲回收 | ✅ |
| React 前端（连接页 / 会话页 / 中英双语） | ✅ |
| **Linux 真实抓屏（X11 + XShm）** | ✅ 增量瓦片、XDamage、无会话时暂停 |
| Linux Wayland / PipeWire 门户抓屏 | ⏳ 未实现（接口已抽象） |
| Windows / macOS 捕获 | ⏳ 未实现 |
| 输入注入 | ⏳ 未实现（协议占位，明确回 `not_implemented`） |
| UDP 直连、WebRTC、P2P、服务端 | ⏳ 二阶段 |
| Tauri 壳层 | 🧩 源码与配置就绪，本机无 Rust 工具链，未编译验证 |

## 架构

```
┌──────────────┐        ┌────────────────────────────────┐
│ Tauri 壳层   │        │ molink-core (C++/libuvcpp)     │
│ React 前端   │◄──────►│ HTTP/WS 127.0.0.1:21118        │
└──────────────┘        │ 捕获/编码 · 输入注入 · UDP     │
                        └────────────────────────────────┘
```

- **molink-core**：C++20 + libuvcpp 后台。负责 HTTP/WS 接口、瓦片帧编码、会话管理；捕获与注入待接入。
- **frontend/**：React 19 + TypeScript + Vite + Tailwind 4 + Zustand + react-i18next。
- **shell/**：Tauri 2 壳层，启动并监管 `molink-core`，把端口与令牌注入前端 URL。
- **server/**、移动端：二阶段。

## 目录

```
molink/
├── .github/workflows/   ci.yml · release-desktop.yml · release-mobile.yml（占位）
├── backend/             molink-core（CMake，libuvcpp）
├── frontend/            React 前端
├── shell/src-tauri/     Tauri 壳层
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
MOLINK_DISPLAY=:1 ./backend/build/molink-core     # 指定 X display
```

抓屏只在有已鉴权会话时运行；没有客户端时进程空闲占用接近 0%。

打开 <http://localhost:1420>，填入令牌连接。也可以直接带参数自动连接：

```
http://localhost:1420/?host=127.0.0.1&port=21118&token=dev-token
```

桌面壳层（需要 Rust + Tauri 2 工具链）：

```bash
cd shell/src-tauri && cargo tauri dev
```

> 本机若出现 `libllhttp.so.9.2: cannot open shared object file`，说明安装的
> `libuvcpp.so` 缺少 `RUNPATH`。本项目的 CMake 已用旧式 `RPATH` 让间接依赖可解析；
> 发布包仍需把 `libuvcpp`、`libllhttp` 等共享库一并打入。

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

推送 `v*` 标签触发 GitHub Actions。一阶段只发布桌面三平台安装包（`.exe`/`.msi`、`.AppImage`/`.deb`、`.dmg`），**不上传任何 zip**。服务端与移动端资产属于二阶段。

## 许可证

MIT。
