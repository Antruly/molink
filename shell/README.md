# moLink 桌面壳层

三个平台各自使用系统 WebView，不打包浏览器引擎，也不需要 Rust。

| 平台 | 技术 | 目录 | 产物 |
|---|---|---|---|
| Linux | GTK 3 + WebKitGTK 4.1 | `native/` | `.deb`、`.AppImage` |
| Windows | Win32 + WebView2 | `windows/` | NSIS `.exe` |
| macOS | Cocoa + WKWebView | `macos/` | `.app`、`.dmg` |

三者行为一致：定位并启动 `molink-core`，读取 `--print-ready` 行，把核心在 `/`
托管的前端加载进 WebView，退出时回收子进程，并支持 `--smoke-test`。

## 查找顺序

| 对象 | 顺序 |
|---|---|
| 核心 | `MOLINK_CORE` → 可执行文件同目录 → 平台资源目录 → 源码树 `backend/build/` |
| 前端 | `MOLINK_WEB_ROOT` → 可执行文件旁的 `web/` → `../Resources/web`（macOS）→ `../share/molink/web` → 源码树 `frontend/dist/` |

## Linux

```bash
sudo apt install -y libgtk-3-dev libwebkit2gtk-4.1-dev
cmake -S shell/native -B shell/native/build -DCMAKE_BUILD_TYPE=Release
cmake --build shell/native/build --parallel
./shell/native/build/moLink
```

快捷键：`F11` 全屏，`Ctrl+Shift+Q` 退出。

## Windows

需要 Visual Studio（CI）或 MinGW-w64，以及 WebView2 SDK（NuGet 包
`Microsoft.Web.WebView2`）：

```powershell
cmake -S shell/windows -B shell/windows/build -DWEBVIEW2_SDK=C:\path\to\wv2
cmake --build shell/windows/build --config Release
```

`WebView2Loader.dll` 会在构建后复制到可执行文件旁，安装器把它一起打包。
Windows 10/11 通常已经带有 WebView2 Runtime。

## macOS

```bash
cmake -S shell/macos -B shell/macos/build -DCMAKE_BUILD_TYPE=Release
cmake --build shell/macos/build --parallel
```

`.app` 布局由 `packaging/macos/build-dmg.sh` 组装：

```
moLink.app/Contents/MacOS/moLink
moLink.app/Contents/MacOS/molink-core
moLink.app/Contents/MacOS/*.dylib
moLink.app/Contents/Resources/web/...
```

## 打包

- Linux：`packaging/linux/build-deb.sh`、`build-appimage.sh`
- Windows：`packaging/windows/stage.sh` + `installer.nsi`（NSIS）
- macOS：`packaging/macos/build-dmg.sh`

发布工作流在三个平台的 runner 上分别执行这些脚本。

## 已知限制

- 只有 Linux 壳在真实环境运行验证过；Windows 壳通过 MinGW 交叉编译，macOS 壳
  等待 CI 验证。
- 屏幕捕获与输入注入目前只有 Linux/X11 后端；Windows/macOS 后端尚未实现。
- macOS 包为 ad-hoc 签名，正式分发需要 Developer ID 与公证。
