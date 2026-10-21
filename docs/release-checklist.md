# 发布检查清单

当前状态：**Linux / X11 功能面达到候选发布（RC）水平；`.deb` 与 `.AppImage` 已验证。
Windows / macOS 壳已实现，但只能在对应 CI runner 或真机上验证。**

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
| 桌面壳 | Xephyr 内 `moLink --smoke-test` | `SHELL_SMOKE_OK title=moLink` |
| .deb | 解包后从暂存目录启动壳层 | 冒烟测试通过 |
| AppImage | `--appimage-extract-and-run --smoke-test` | 冒烟测试通过 |
| Windows 壳 | MinGW-w64 交叉编译 | 链接通过 |

## 发布前仍需完成

1. **构建环境**
   - `libgtk-3-dev`、`libwebkit2gtk-4.1-dev`（壳层）
   - libuvcpp 源码（CI 使用 `UVCPP_REPO` 仓库变量）
   - `dpkg-deb`（`.deb`）；AppImage 需要 `appimagetool` 与 `patchelf`
2. **平台与后端**
   - Windows：DXGI 抓屏、SendInput、光标、剪贴板已实现，需真机回归
   - macOS：CoreGraphics 抓屏、CGEvent、NSPasteboard 已实现，需真机回归；权限与光标待补
   - Wayland：PipeWire + xdg-desktop-portal ScreenCast（需 `libpipewire-0.3-dev` 与 D-Bus 开发库）
   - AppImage 产物（当前只有 `.deb`）
3. **发布产物**
   - Linux `.deb` / `.AppImage`：已生成并验证
   - Windows NSIS `.exe`：脚本就绪，待 CI 验证
   - macOS `.dmg`：脚本就绪，待 CI 验证；ad-hoc 签名，正式分发需 Developer ID
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

- 本机通过临时解包的 WebKitGTK 开发包编译了 Linux 壳层；正式构建环境仍需
  `libgtk-3-dev` 与 `libwebkit2gtk-4.1-dev`。
- Windows/macOS 后端只能交叉编译（Windows）或交给 CI（macOS），尚未在真实平台运行。
- macOS 未实现远端光标位图，正式发布前应补齐或接受只有本地光标。
- 没有 PipeWire/D-Bus 开发头文件：Wayland 路径无法在本机构建。
- 代码仓库没有配置远端：无法创建 tag 或上传 Release。

当前可交付：**Linux/X11 的 `.deb` 与 `.AppImage`**。三平台安装包在 CI 上可生成，
但 Windows/macOS 运行时行为需要真机确认。
