# 发布检查清单

当前状态：**Linux / X11 功能面达到候选发布（RC）水平；三平台安装包尚不可生成。**

## 已验证

| 项目 | 命令 / 方法 | 结果 |
|---|---|---|
| 协议编解码 | `ctest --test-dir backend/build` | 36/36 |
| 后台构建 | `cmake -S backend -B backend/build && cmake --build backend/build` | 通过 |
| 前端 | `pnpm lint && pnpm typecheck && pnpm build` | 通过 |
| HTTP/鉴权 | `uvp_verify`（health、`/api/*` 401、`/`） | verified |
| 抓屏 | 真实桌面 2880×1920，X11 + XShm + XDamage | 通过 |
| 输入 | Xephyr 内探针收到 motion/button/wheel/Shift+A | 通过 |
| 光标 | XFixes 位图 + 位置；CDP 校验覆盖层位置/缩放 | 通过 |
| 剪贴板 | X11 探针双向 set/get | 通过 |
| 压缩 | 画布中心像素与源窗口 `#002244` 一致 | 通过 |
| 带宽/CPU | 2880×1920 活跃会话 | ~7.4 Mbps / ~24% 单核 |

## 发布前仍需完成

1. **构建环境**
   - Rust stable + Tauri 2 CLI
   - `libwebkit2gtk-4.1-dev`、`libappindicator3-dev`、`librsvg2-dev`、`patchelf`
   - libuvcpp 源码（CI 使用 `UVCPP_REPO` 仓库变量）
2. **平台后端**
   - Windows：DXGI Desktop Duplication 抓屏、SendInput 注入、光标/剪贴板
   - macOS：ScreenCaptureKit、CGEvent 注入、辅助功能权限
   - Wayland：PipeWire + xdg-desktop-portal ScreenCast（需 `libpipewire-0.3-dev` 与 D-Bus 开发库）
3. **发布产物**
   - Windows `.exe` / `.msi`
   - Linux `.AppImage` / `.deb`（含 libuvcpp、libllhttp 等共享库；核心使用 `$ORIGIN` RPATH）
   - macOS `.dmg`
   - 不发布桌面端 zip
4. **人工回归**
   - 多显示器与分辨率切换
   - 长会话（>1 小时）内存与句柄稳定性
   - 断线重连、并发两个客户端
   - 高 DPI 与不同键盘布局
   - 中文输入法组合输入
5. **安全**
   - 令牌只出现在本地回环；检查日志不泄露令牌
   - 剪贴板与输入注入均有能力开关，默认行为符合预期
   - MIT 许可与版权声明随包分发

## 当前阻塞

- 本机没有 Rust/Tauri 与 webkit2gtk 开发包：无法编译壳层或生成安装包。
- 没有 Windows/macOS 机器或交叉工具链：平台后端无法实现与验证。
- 没有 PipeWire/D-Bus 开发头文件：Wayland 路径无法在本机构建。
- 代码仓库没有配置远端：无法创建 tag 或上传 Release。

在上述条件具备前，可交付的是 **Linux/X11 源码构建**（`molink-core` + `frontend/dist`），
不是文档中定义的三平台安装包。
