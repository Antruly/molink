# moLink 远程桌面 · 开发说明文档

> 版本：0.2（架构基线 + 仓库/CI/发布）
> 定位：Web 优先的跨平台远程控制软件。一套后台逻辑 + 浏览器壳层，桌面端直连，移动端经服务端中转，二阶段引入 ID 服务器与 P2P。
> 命名：**moLink**（小写 m，大写 L）。对外品牌名写作 `moLink`，二进制/包名/目录使用小写 `molink`。
> 许可证：**MIT**。桌面端与移动端在本仓库发布；服务端自 0.2.1 起拆分到独立仓库 `molink-server`，拥有独立的版本线与发布流程。

---

## 0. 命名规范

| 用途 | 标识 |
|---|---|
| 产品名 | `moLink` |
| 后台可执行文件 | `molink-core` |
| 壳层可执行文件 | `moLink`（Windows: `moLink.exe`） |
| 仓库根目录 | `molink/` |
| 本地后台健康检查 `service` 字段 | `moLink Core` |
| 自定义 URI scheme | `molink://` |
| 深链接示例 | `molink://session/A3F-9K2-X7Q`、`molink://connect?host=192.168.1.10&port=28418` |
| 托盘/菜单显示名 | `moLink` |
| 服务端产品名（二阶段） | `moLink Server` |
| 服务端 Web 品牌名 | `moLink Web` |
| 配置目录（Linux） | `~/.config/molink/` |
| 配置目录（Windows） | `%APPDATA%\moLink\` |
| 配置目录（macOS） | `~/Library/Application Support/moLink/` |
| 帧包 Magic 字段 | `"MOLK"`（**不可更改**，协议契约） |

**术语约定**：“壳层”指 Tauri 浏览器壳；“后台”指 `molink-core` 进程；“前端”指壳层内加载的 Web 页面；“服务端”仅指二阶段的 `moLink Server`。避免用“客户端”同时指代壳层和后台。

---

## 1. 前端框架决策：React

### 1.1 结论

**前端采用 React 19 + TypeScript。** Vue 3 是同样合理的选择，两者在 Tauri 壳层下完全等价——Tauri 只加载 `devUrl` 或 `frontendDist`，对框架无感知[reference:0]。选择 React 的核心理由与远程桌面场景直接相关，而非框架优劣之争：

**第一，远程会话页面的状态复杂度高于普通管理后台。** 视频渲染状态、连接状态、输入模式、码流元数据、延迟/帧率指标需要在一个页面内协调更新。React 的不可变状态模型配合 **Zustand** 的细粒度 selector，可以精确控制哪些组件因哪个状态切片变化而重渲染，避免视频渲染组件被无关状态更新拖累。Vue 3 的响应式系统同样能做到，但 React 生态中围绕 **WebCodecs + Canvas/WebGPU 渲染管线** 的参考实现和社区讨论更多[reference:1]。远程桌面是 WebCodecs 的重度场景，生态成熟度有实际价值。

**第二，shadcn/ui + Tailwind 的组合是“复制源码而非黑盒依赖”的组件策略**，与 moLink 需要深度定制主题令牌（墨绿/青绿信号色、深色指挥中心风格）的需求高度契合。这套组合在 React 侧是一等公民。

Vue 3 的优势在于单文件组件的心智负担更低，如果团队已有 Vue 经验，切换成本可以忽略。但就“远程桌面 + WebCodecs + 精细状态控制”这一特定场景，React 的生态匹配度略高。

### 1.2 完整前端技术栈

| 层 | 选择 |
|---|---|
| 框架 | **React 19 + TypeScript** |
| 构建 | **Vite** |
| 状态管理 | **Zustand** |
| 样式 | **Tailwind CSS + CSS 自定义属性** |
| 国际化 | **react-i18next** |
| 视频渲染 | **WebCodecs + Canvas/WebGPU**；WebRTC 路径用 `<video>` |
| 组件基座 | **shadcn/ui**（Radix + Tailwind） |

---

## 2. 总体架构

### 2.1 核心设计原则

**一套后台，多种壳。** 所有平台共享同一份 `molink-core`（C++/libuvcpp）和同一套 React 前端。桌面端（Windows / Linux / macOS）是 **Tauri 壳层 + `molink-core` 后台进程** 的组合：壳层加载 `http://127.0.0.1:{port}`，后台负责捕获、编码、传输和输入注入。移动端（Android / iOS）不运行后台，Tauri 壳层直接加载 **moLink Web**，仅作为控制端。

**直连优先，中转兜底。** 桌面端之间在局域网内通过 IP 直连，不经服务器。二阶段 ID 服务器只承担信令和 NAT 穿透辅助，媒体流尽量 P2P；P2P 失败回退中转。

**传输层双模：自定义 UDP 用于客户端直连，WebRTC 用于浏览器互通。** 两条路径共享同一套瓦片码流格式。WebRTC 路径将帧包通过 `RTCDataChannel` 承载，而非封装进 RTP 视频轨。

### 2.2 组件拓扑
┌─────────────────────────────────────────────────────────────┐
│ 桌面客户端（Windows / Linux / macOS） │
│ ┌──────────────┐ ┌─────────────────────────────────────┐ │
│ │ Tauri 壳层 │ │ molink-core（C++/libuvcpp） │ │
│ │ React 前端 │◄──┤ · HTTP/WS (127.0.0.1:port) │ │
│ │ 加载 127.0.0.1│ │ · 瓦片捕获/编码引擎 │ │
│ └──────────────┘ │ · 输入注入 · 直连 UDP │ │
│ └─────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
│
┌───────────────┴───────────────┐
▼ ▼
┌─────────────────────────┐ ┌─────────────────────────────────┐
│ 另一台桌面客户端 │ │ moLink Server（二阶段） │
│ 直连 / P2P / 中转 │ │ · ID 服务器 · TURN · 用户系统 │
└─────────────────────────┘ │ · moLink Web │
└─────────────────────────────────┘

text

### 2.3 阶段划分与发布策略

| 阶段 | 范围 | Release 资产 |
|---|---|---|
| **一阶段** | 桌面三平台直连 + 局域网 Web 预览 | Windows `.exe`/`.msi`，Linux `.AppImage`/`.deb`，macOS `.dmg`；**不发布 zip** |
| **二阶段** | 服务端 + 移动端 | 服务端在独立仓库 `molink-server` 发布（自有 `v*` 标签）：Linux `.deb`/`.rpm`/`.tar.gz`（systemd）、Windows 安装器 + `.zip`（可选服务）、macOS `.pkg`/`.tar.gz`（launchd）；Android `.apk`/`.aab`，iOS `.ipa` |

**“不发布 zip”的含义**：Release 资产中不上传任何 `.zip` 格式的桌面客户端包。Windows 使用 Inno Setup `.exe` 安装包与免安装单文件 `.exe`，Linux 使用 AppImage（可执行单文件）和 `.deb`，macOS 使用 `.dmg`。二阶段服务端例外，允许 zip 作为便捷分发形态。

---

## 3. 仓库初始化

### 3.1 创建仓库

在 GitHub 创建仓库 `molink`，**不要初始化 README、.gitignore 或 LICENSE**，保持空仓库。本地初始化后首次推送：

```bash
mkdir molink && cd molink
git init
git remote add origin git@github.com:{your-org}/molink.git
3.2 目录结构（初始）
text
molink/
├── .github/
│   └── workflows/
│       ├── ci.yml                 # PR 构建验证（三平台编译 + 前端 lint/test）
│       ├── release-desktop.yml    # 桌面三平台发布（一阶段）
│       └── release-mobile.yml     # 移动端发布（二阶段，先占位）
├── frontend/                      # React 前端
├── shell/                         # Tauri 壳层（桌面）
│   └── src-tauri/
├── backend/                       # molink-core（C++）
├── docs/
├── README.md
├── LICENSE
└── .gitignore
3.3 .gitignore
gitignore
# Rust / Tauri
shell/src-tauri/target/
**/*.rs.bk
Cargo.lock

# Node / 前端
node_modules/
frontend/dist/
.pnpm-store/

# C++ 构建
backend/build/
backend/cmake-build-*/
*.o
*.a
*.so
*.dylib
*.dll

# IDE
.vscode/
.idea/
*.swp

# 本地配置与密钥
.env
.env.local
*.jks
*.keystore
*.p12
*.mobileprovision
4. README.md（模板）
markdown
# moLink

跨平台远程桌面。Web 优先，一套后台 + 浏览器壳层，桌面端直连，移动端经服务端中转。

## 架构

- **molink-core**：C++/libuvcpp 后台，负责屏幕捕获、瓦片编码、直连传输、输入注入。
- **前端**：React 19 + TypeScript + Vite，通过 `http://127.0.0.1:{port}` 与本地后台通信。
- **Tauri 壳层**：桌面三平台的浏览器壳，生命周期与 `molink-core` 绑定。
- **moLink Server**（二阶段）：ID 服务器、TURN 中转、moLink Web 控制台。

## 开发

### 前置依赖

- Node.js LTS + pnpm
- Rust stable + Tauri 2.0 工具链
- CMake + C++20 编译器（backend）
- 各平台捕获依赖：Windows（无额外依赖）、Linux（PipeWire）、macOS（ScreenCaptureKit）

### 本地启动

```bash
# 前端
cd frontend && pnpm install && pnpm dev

# 后端（开发模式）
cd backend && cmake -B build && cmake --build build

# 壳层
cd shell/src-tauri && cargo tauri dev
构建
bash
# 桌面安装包
pnpm tauri build

# Android（二阶段）
pnpm tauri android build

# iOS（二阶段，需 macOS）
pnpm tauri ios build
发布
推送 v* 标签触发 GitHub Actions，自动构建三平台安装包并创建 Release。一阶段不上传 zip，仅上传安装包格式。

许可证
MIT

text

---

## 5. MIT 许可证

在仓库根目录创建 `LICENSE`：

```text
MIT License

Copyright (c) 2026 moLink Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
唯一义务：任何分发形态（源码、二进制、安装包）都必须保留此版权声明和许可声明。MIT 不传染，moLink 的衍生作品可以按任何许可证分发，包括闭源商业产品。

6. CI 工作流
6.1 PR 验证（ci.yml）
触发条件：pull_request 到 main。目标：三平台编译通过 + 前端 lint/typecheck 通过，不创建 Release。

yaml
name: CI

on:
  pull_request:
    branches: [main]

jobs:
  frontend:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: pnpm/action-setup@v4
        with:
          version: 9
      - uses: actions/setup-node@v4
        with:
          node-version: lts/*
          cache: pnpm
      - run: pnpm install --frozen-lockfile
        working-directory: frontend
      - run: pnpm lint && pnpm typecheck
        working-directory: frontend

  backend:
    strategy:
      matrix:
        os: [ubuntu-latest, windows-latest, macos-latest]
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/checkout@v4
      - name: Install Linux deps
        if: matrix.os == 'ubuntu-latest'
        run: sudo apt-get update && sudo apt-get install -y cmake libuv1-dev
      - name: Configure
        run: cmake -B backend/build
      - name: Build
        run: cmake --build backend/build --config Release

  shell-check:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: dtolnay/rust-toolchain@stable
      - name: Install Tauri Linux deps
        run: |
          sudo apt-get update
          sudo apt-get install -y libwebkit2gtk-4.1-dev libappindicator3-dev librsvg2-dev
      - name: Cargo check
        run: cargo check
        working-directory: shell/src-tauri
6.2 桌面发布（release-desktop.yml，一阶段启用）
触发条件：推送 v* 标签（如 v0.1.0）。使用 tauri-apps/tauri-action 构建并自动创建 GitHub Release。矩阵覆盖 Windows x64、Linux x64、Linux Arm64、macOS x64、macOS Arm64，通过 args 指定 Rust target。

yaml
name: Release Desktop

on:
  push:
    tags: ['v*']

permissions:
  contents: write

jobs:
  publish-tauri:
    strategy:
      fail-fast: false
      matrix:
        include:
          - platform: macos-latest
            args: '--target aarch64-apple-darwin'
          - platform: macos-latest
            args: '--target x86_64-apple-darwin'
          - platform: ubuntu-22.04
            args: '--target x86_64-unknown-linux-gnu --bundles appimage,deb'
          - platform: windows-latest
            args: '--target x86_64-pc-windows-msvc'

    runs-on: ${{ matrix.platform }}
    steps:
      - uses: actions/checkout@v4

      - name: Install Linux deps
        if: matrix.platform == 'ubuntu-22.04'
        run: |
          sudo apt-get update
          sudo apt-get install -y libwebkit2gtk-4.1-dev libappindicator3-dev librsvg2-dev patchelf
          sudo apt-get install -y cmake libuv1-dev

      - uses: pnpm/action-setup@v4
        with: { version: 9 }
      - uses: actions/setup-node@v4
        with: { node-version: lts/*, cache: pnpm }

      - uses: dtolnay/rust-toolchain@stable
        with:
          targets: ${{ matrix.platform == 'macos-latest' && 'aarch64-apple-darwin,x86_64-apple-darwin' || '' }}
      - uses: swatinem/rust-cache@v2
        with:
          workspaces: shell/src-tauri

      - name: Build backend
        run: cmake -B backend/build -DCMAKE_BUILD_TYPE=Release && cmake --build backend/build --config Release

      - run: pnpm install --frozen-lockfile
        working-directory: frontend
      - run: pnpm build
        working-directory: frontend

      - uses: tauri-apps/tauri-action@v1
        env:
          GITHUB_TOKEN: ${{ secrets.GITHUB_TOKEN }}
        with:
          projectPath: shell
          tagName: ${{ github.ref_name }}
          releaseName: moLink ${{ github.ref_name }}
          releaseBody: 'See assets for platform installers.'
          releaseDraft: false
          prerelease: false
          args: ${{ matrix.args }}
Linux Arm64 的补充：AppImage 工具链的跨架构支持有缺口，可通过 pguyot/arm-runner-action 在模拟 Arm 环境中直接编译。一阶段可以先将 Linux Arm64 标记为“实验性”，在矩阵中增加：

yaml
- platform: ubuntu-22.04
  args: '--target aarch64-unknown-linux-gnu --bundles appimage,deb'
  use_arm_runner: true
对应步骤中在 tauri-action 之前插入 pguyot/arm-runner-action 的 Arm 环境准备。如果一阶段追求简化，可以暂不发布 Linux Arm64，待 CI 稳定后追加。

“不上传 zip”的落实：tauri-action 只会扫描并上传平台特定的 bundle（.dmg、.msi/.exe、.AppImage/.deb），不会自动生成 zip。Windows 侧确保 Inno Setup 安装包（以及 NSIS 免安装单文件）已配置，不要额外添加 zip 打包步骤。

6.3 移动端发布（release-mobile.yml，二阶段启用）
Android
Android 签名发布需要四个 GitHub Actions secrets：ANDROID_KEYSTORE_BASE64、ANDROID_KEYSTORE_PASSWORD、ANDROID_KEY_ALIAS、ANDROID_KEY_PASSWORD。工作流在推送 v* 标签时构建签名 APK/AAB，并附加到同一 Release。

yaml
name: Release Mobile

on:
  push:
    tags: ['v*']

permissions:
  contents: write

jobs:
  android:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-java@v4
        with: { distribution: 'temurin', java-version: '17' }
      - uses: pnpm/action-setup@v4
        with: { version: 9 }
      - uses: actions/setup-node@v4
        with: { node-version: lts/*, cache: pnpm }
      - uses: dtolnay/rust-toolchain@stable
        with: { targets: 'aarch64-linux-android,armv7-linux-androideabi,x86_64-linux-android,i686-linux-android' }

      - run: pnpm install --frozen-lockfile && pnpm build
        working-directory: frontend

      - name: Decode keystore
        run: echo "${{ secrets.ANDROID_KEYSTORE_BASE64 }}" | base64 -d > $RUNNER_TEMP/release.jks
      - name: Build signed APK
        env:
          ANDROID_KEYSTORE_PATH: ${{ runner.temp }}/release.jks
          ANDROID_KEYSTORE_PASSWORD: ${{ secrets.ANDROID_KEYSTORE_PASSWORD }}
          ANDROID_KEY_ALIAS: ${{ secrets.ANDROID_KEY_ALIAS }}
          ANDROID_KEY_PASSWORD: ${{ secrets.ANDROID_KEY_PASSWORD }}
        run: pnpm tauri android build --apk --aab
        working-directory: shell

      - uses: softprops/action-gh-release@v2
        with:
          files: |
            shell/gen/android/app/build/outputs/apk/release/*.apk
            shell/gen/android/app/build/outputs/bundle/release/*.aab
iOS
iOS 构建需要 macOS runner、Apple 开发者证书和 provisioning profile。建议二阶段初期使用 TestFlight 作为分发渠道，而非直接在 GitHub Release 附带 .ipa（后者需要企业证书或 Ad Hoc 描述文件，管理复杂度更高）。如果坚持 Release 附带 .ipa，需要配置 IOS_CERTIFICATE_BASE64、IOS_PROVISIONING_PROFILE_BASE64、IOS_TEAM_ID 等 secrets，并使用 xcodebuild 或 Fastlane 导出签名后的 .ipa。

7. 发布矩阵总结
平台	架构	格式	阶段	CI Runner
Windows	x64	.exe（Inno Setup）/ 免安装 .exe	一阶段	windows-latest
Linux	x64	.AppImage + .deb	一阶段	ubuntu-22.04
Linux	Arm64	.AppImage + .deb	一阶段（实验性）/ 后续	ubuntu-22.04 + Arm runner
macOS	x64	.dmg	一阶段	macos-latest
macOS	Arm64	.dmg	一阶段	macos-latest
Android	arm64-v8a 等	.apk + .aab	二阶段	ubuntu-latest
iOS	arm64	.ipa（TestFlight 或 Ad Hoc）	二阶段	macos-latest
服务端	x64	.deb / .rpm / .tar.gz；.exe + .zip；.pkg / .tar.gz	二阶段	独立仓库 `molink-server`（v*）
桌面端与移动端从本仓库的标签构建；服务端已拆分到独立仓库 `molink-server`，使用该仓库自己的 `v*` 标签与发布工作流，避免两套版本线互相牵连。第一阶段 Release 不包含移动端资产。

8. 关键工程决策与风险
决策/风险	处理
React vs Vue3	选择 React。Tauri 对两者无感知，但 React + Zustand + shadcn/ui + WebCodecs 生态匹配度更高。
不上传 zip	tauri-action 默认只上传平台 bundle；不添加额外 zip 步骤。服务端二阶段例外。
Linux Arm64 AppImage	使用 arm-runner-action 模拟 Arm 环境编译；一阶段可标记实验性。
壳层孤儿进程	双向心跳 + IPC 管道 EOF 检测；壳层强制死亡时 molink-core 超时自退出。
编码输出格式是唯一契约	自定义 UDP 和 WebRTC RTCDataChannel 共享同一套瓦片帧包，只改变承载方式。
GPL/AGPL 污染	libuvcpp、Zstd、LZ4、Tauri 均为宽松许可；不复制 RustDesk 代码，仅参照公开架构思想独立实现。
MIT 合规	所有分发资产附带 LICENSE；molink-core 和壳层二进制均保留版权声明。
text

这份文档在上一版架构基线之上，补齐了**仓库初始化步骤、README 模板、MIT 协议全文、PR 验证与桌面/移动端 Release 工作流**，并明确了一阶段“不上传 zip”和移动端二阶段签名的具体配置。前端框架决策选择了 React，理由集中在远程桌面场景下 WebCodecs 生态和细粒度状态控制的匹配度，而非框架优劣本身。CI 部分的关键验证点是：`tauri-action` 的 bundle 扫描机制天然满足“不上传 zip”的要求，移动端发布的核心复杂度在签名 secrets 的配置而非工作流逻辑本身。
