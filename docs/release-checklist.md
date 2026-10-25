# 发布检查清单

当前状态：**桌面 0.9.18（标签 `v0.9.18`）+ 服务端 0.2.2（独立仓库 `molink-server`）+
Windows 7 x86（同一个 Release 的 `win7-x86` job）。桌面端已验证
Linux/Windows/macOS 发布矩阵、简洁模式/头像/域组码与 Win7 GDI 采集；服务端目录、
TLS、域组码和迁移在独立仓库验证。** 历史条目（0.6.14 及以下）保留在下方作为回归
基线；Win7 x86 的专项检查见文末。

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
| 设备页 | 设备 ID/验证码、隐藏/复制、能力状态 | 截图验证 |
| 多会话 | 首次独立窗口、后续 Tab、标签拖出 | Linux 壳内实测 |
| 主题 | 亮/暗切换与持久化 | 截图验证 |
| 无边框窗口 | 圆角、自绘标题栏、拖拽/缩放、托盘 | Linux 运行；Windows 编译 |
| 本机免验证 | 壳层/浏览器直达主页 | 通过 |
| 远程登录 | 局域网 IP 打开 → 验证码 → 主页 | 通过 |
| 远程控制主机 | 主页点击“控制这台设备” | 通过 |
| 空闲锁定 | `--lock-timeout 2`，锁定后 API 423 / WS 1008 | 通过 |
| 会话抑制锁定 | `/api/ui-state` 报告会话后不锁定 | 通过 |
| Windows 壳 | MinGW-w64 交叉编译（含图标资源） | 链接通过 |
| 画质预设 | `tools/bench_quality.py`（1600×900 动画） | 全部 PASS；流畅带宽 35%、关键帧 20% |
| 编码预算 | 2880×1920 关键帧 / 增量帧 | 17–18 ms / 13–18 ms，全部低于帧预算 |
| 解码预算 | Chrome，2880×1920 | 增量帧 6.4 / 9.7 ms；关键帧主线程扩展 ≤ 24.1 ms |
| 退出稳定性 | 4 次 SIGTERM 压力测试 | 0 次崩溃 |
| Windows 服务采集 | 服务安装后 `welcome.tiles.backend=secure-desktop`；helper 记录 `dpi`；`can_set_display_mode=false` 时客户端隐藏分辨率菜单 | 通过（Win10 实机，0.9.18 构建） |
| 多次码重连 | `session_token` 重连与重复 `hello` 不扣次数（18→14 仅两次验证码连接） | 通过（Win10 实机） |

## 发布前仍需完成

1. **构建环境**
   - `libgtk-3-dev`、`libwebkit2gtk-4.1-dev`（壳层）
   - libuvcpp 预编译包（Linux/Windows，`UVCPP_VERSION`，默认 1.8.5）；
     macOS 使用 `UVCPP_REPO` / `UVCPP_REF` 从源码构建
   - `dpkg-deb`（`.deb`）；AppImage 需要 `appimagetool` 与 `patchelf`
   - Windows：Inno Setup 6（安装包）与 NSIS（免安装单文件），CI 自动安装
2. **平台与后端**
   - Windows：DXGI 抓屏、SendInput、光标、剪贴板已实现，需真机回归
   - macOS：CoreGraphics 抓屏、CGEvent、NSPasteboard 已实现，需真机回归；权限与光标待补
   - Wayland：PipeWire + xdg-desktop-portal ScreenCast（需 `libpipewire-0.3-dev` 与 D-Bus 开发库）
   - AppImage 产物（当前只有 `.deb`）
3. **发布产物**
   - Linux `.deb` / `.AppImage`：已生成并验证
   - Windows Inno Setup 安装包 `.exe` + 免安装单文件 `.exe`：脚本就绪，待 CI 验证
   - macOS `.dmg`：脚本就绪，待 CI 验证；ad-hoc 签名，正式分发需 Developer ID
   - 不发布桌面端 zip
4. **人工回归**
   - 多显示器与分辨率切换
   - 长会话（>1 小时）内存与句柄稳定性
   - 断线重连、并发两个客户端
   - 局域网访问：默认监听 `0.0.0.0`、Windows 防火墙规则生效、错误验证码触发阶梯锁定/封禁
   - 单实例：重复启动聚焦已有窗口，不再启动第二个核心；托盘隐藏后再次启动也能聚焦
   - 安全默认：仅回环、普通浏览器访问关闭、发现关闭；壳层 `client_token` 可正常打开 UI，普通浏览器无 token 时 403
   - 授权码：长期码校验、单次码消费换新、多次码扣减/耗尽换新、自定义多次码
   - 高 DPI 与不同键盘布局
   - 中文输入法组合输入
5. **安全**
   - 验证码经局域网明文 HTTP 传输，仅应在可信网络使用；检查日志不泄露令牌
   - 剪贴板与输入注入均有能力开关，默认行为符合预期
   - MIT 许可与版权声明随包分发

## 当前阻塞

- 本机通过临时解包的 WebKitGTK 开发包编译了 Linux 壳层；正式构建环境仍需
  `libgtk-3-dev` 与 `libwebkit2gtk-4.1-dev`。
- Windows/macOS 后端只能交叉编译（Windows）或交给 CI（macOS），尚未在真实平台运行。
- macOS 未实现远端光标位图，正式发布前应补齐或接受只有本地光标。
- 没有 PipeWire/D-Bus 开发头文件：Wayland 路径无法在本机构建。

当前可交付：**Linux/X11 的 `.deb` 与 `.AppImage`**。三平台安装包在 CI 上可生成，
但 Windows/macOS 运行时行为需要真机确认。

## Windows 7 x86（预览，2026-10-25）

| 项目 | 命令 / 方法 | 结果 |
|---|---|---|
| 构建 | `packaging/windows/build-win7-x86.ps1`（libuvcpp full + core + shell + service/helper + 安装器） | 通过 |
| CI | `release-desktop.yml` 的 `win7-x86` job（`v*` 标签） | 通过（随 0.9.18 Release 构建） |
| 目录 / 域组 | 局域网 TLS 目录注册、ID 解析、域组码 | 通过 |
| WS 会话 | 反向隧道 → welcome → 129 KB 真实关键帧 | 通过 |
| GDI 采集 | 1024×768 控制台会话，`capture=gdi` | 通过 |
| 输入 / 光标 | SendInput；`cursor` 32×32 RGBA + `cursor-position` | 通过 |
| 音频 | 虚拟机无声卡设备，欢迎消息 `audio=false` | 预期行为 |
| 安装器 | Inno Setup 编译 | 通过（未在 Win7 上以 UAC 实际安装） |
| 防火墙 / 服务 | `install-firewall.cmd` / `install-service.cmd` | 需管理员运行，未在过滤令牌会话中验证 |
