# moLink 壳层

Tauri 2 桌面壳。启动时：

1. 按顺序查找 `molink-core`：`MOLINK_CORE` 环境变量 → 安装包的 resource 目录 →
   可执行文件同目录 → 开发期的 `backend/build/`。
2. 以 `--print-ready --parent-pid <shell pid>` 启动核心。
3. 读取核心的 ready JSON，得到端口与令牌。
4. 创建主窗口，加载 `index.html?host=127.0.0.1&port=…&token=…`，前端据此自动连接。
5. 退出时结束子进程；壳层被强杀时由核心的父进程监视兜底。

## 开发

需要 Rust stable 与 Tauri 2 工具链，以及完成构建的 `backend/build/molink-core`。

```bash
cd shell/src-tauri && cargo tauri dev
```

前端 dev server 由 `beforeDevCommand` 自动启动（固定 1420 端口）。

## 打包

```bash
pnpm --dir frontend build
# 先把 molink-core（Linux 还要带上 libuvcpp.so、libllhttp.so 等）放进
# shell/src-tauri/binaries/
cd shell/src-tauri && cargo tauri build
```

`binaries/*` 作为资源随包安装。Linux 下核心以 `$ORIGIN` 为 RPATH，因此共享库与
核心放在同一目录即可被找到。

## 图标

`src-tauri/icons/` 由仓库内的几何图形生成（深色圆角底 + 青绿显示器），包含 PNG、
ICO 与 ICNS。重新生成时需要保持同一套尺寸。

## 状态

源码与配置完整，但本机没有 Rust 工具链，尚未编译验证；首次构建请关注 Tauri 2
API 与 `tauri.conf.json` 的版本兼容性。
