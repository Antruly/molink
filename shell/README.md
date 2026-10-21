# moLink 桌面壳层（Linux）

C++20 + GTK 3 + WebKitGTK 4.1。启动 `molink-core`，读取它的 ready 行，然后把核心
在 `/` 托管的前端加载进系统 WebView。不打包浏览器引擎，也不需要 Rust。

## 构建

```bash
sudo apt install -y libgtk-3-dev libwebkit2gtk-4.1-dev
cmake -S shell/native -B shell/native/build -DCMAKE_BUILD_TYPE=Release
cmake --build shell/native/build --parallel
```

运行前需要有已经构建好的：

- `backend/build/molink-core`
- `frontend/dist`（`cd frontend && pnpm build`）

查找顺序（可用环境变量覆盖）：

| 对象 | 顺序 |
|---|---|
| 核心 | `MOLINK_CORE` → 可执行文件同目录 → `../lib/molink/` → 源码树 `backend/build/` → 当前目录 |
| 前端 | `MOLINK_WEB_ROOT` → 可执行文件同目录 `/web` → `../web` → `../share/molink/web` → 源码树 `frontend/dist/` |

## 运行

```bash
./shell/native/build/moLink
```

- 核心以 `--port 0` 启动（系统分配端口），壳层从 ready 行取得端口与令牌。
- 壳层把 `--parent-pid <shell pid>` 传给核心；壳层被杀时核心会自行退出。
- 快捷键：`F11` 全屏，`Ctrl+Shift+Q` 退出。
- `--smoke-test`：启动、等待页面加载完成、打印 `SHELL_SMOKE_OK title=...` 后退出，
  用于 CI 或无头验证。

## 打包

```bash
./packaging/linux/build-deb.sh
# build/package/molink_0.1.0_amd64.deb
```

安装布局：

```
/opt/molink/bin/moLink
/opt/molink/bin/molink-core
/opt/molink/web/...            # frontend/dist
/opt/molink/lib/libuvcpp.so
/opt/molink/lib/libllhttp.so*
/usr/bin/moLink -> /opt/molink/bin/moLink
/usr/share/applications/molink.desktop
```

核心的 RPATH 是 `$ORIGIN:$ORIGIN/../lib`，因此随包的 libuvcpp/libllhttp 会被优先
找到。系统依赖（GTK、WebKitGTK、X11、XTEST、XFixes、XDamage）写在 deb 的
`Depends` 里。

## 平台范围

当前壳层只支持 Linux。Windows 需要基于 WebView2 的原生壳，macOS 需要
WKWebView 壳；两者尚未实现。
