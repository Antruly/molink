# Wayland 支持现状

当前 Linux 采集与输入只有 X11 后端（`x11_capture.cpp` / `x11_input.cpp`）。
在 Wayland 会话中这些后端连到的是 **XWayland**，因此只能看到、也只能操作
XWayland 应用；原生 Wayland 应用不在 X server 里，既抓不到也点不动。

Y1 的目标是**把这件事说清楚**，不再静默降级：core 启动时判断会话类型，诊断
日志与 `/api/state` 都会上报，本机 UI 显示一条可关闭的提示。采集与注入路径
本身没有改变。

## 能做什么

| 能力 | Wayland 会话下的现状 |
|---|---|
| 屏幕采集 | 仅 XWayland 窗口（X11 screen capture） |
| 鼠标 / 键盘注入 | 仅对 XWayland 应用有效（XTEST） |
| 剪贴板 | 仅 X11 选区（XWayland 应用之间） |
| 显示器列表 / 缩放 | 由 XWayland 的 X RandR 决定，可能与真实 Wayland 输出不一致 |
| 组合键 | **可能不可用**：X11 抓取（XGrabKeyboard / XIGrabDevice）拿不到 Wayland compositor 的全局快捷键，例如 Super、Alt+Tab 仍由桌面处理 |

原生 Wayland 应用（GNOME 的终端、文件管理器、Firefox 的 Wayland 后端等）
不会出现在画面里，远程输入也到不了它们。这不是故障提示，而是当前实现边界。

## 检测规则

`backend/src/display/session_type.h` 中的纯函数按以下优先级判断：

1. `XDG_SESSION_TYPE=wayland` → `wayland`（即使 `DISPLAY` 已由 XWayland 设置）
2. `WAYLAND_DISPLAY` 非空且 `DISPLAY` 为空 → `wayland`
3. `XDG_SESSION_TYPE=x11`，或只有 `DISPLAY` → `x11`
4. 其余（包括两个 display 变量同时存在但没有 XDG 值）→ `unknown`

上报位置：

- 诊断日志：`display.session type=<x11|wayland|unknown> capture=<x11|xwayland_only|unknown> input=<...>`
- Wayland 下额外写一条 `display.wayland` 警告，并在 stderr 提示一次
- `GET /api/state` 的 `session_type` 字段；本机 UI 据此显示提示条，关闭后
  （`localStorage` 键 `molink.waylandNoticeDismissed`）不再出现

## 后续计划

- **Y2（采集）**：接入 `xdg-desktop-portal` 的 **ScreenCast**（PipeWire 流），
  先走内存帧喂现有编码器，再考虑 dmabuf 零拷贝。portal 需要用户在每次会话
  的弹窗里选择屏幕，无法静默授权。
- **Y3（输入）**：接入 `xdg-desktop-portal` 的 **RemoteDesktop**（EIS /
  libei）注入指针与键盘；没有 portal 时再考虑 `/dev/uinput` 兜底。
- **剪贴板**：wlroots 系（Sway/Hyprland）可走 `wlr-data-control`；GNOME/KDE
  需要 portal 或继续标注「仅 XWayland」。
- **组合键**：EIS 能发送 keysym，但最终是否生效由 compositor 决定。能发则发，
  不能就明确禁用并提示，不做假动作。

在这些完成之前，Wayland 会话下的 moLink 仍以「XWayland 应用」为限；需要同步
原生应用时，请使用 X11 会话登录桌面。
