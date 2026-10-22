# Windows 会话 / 虚拟显示器 / 实时剪贴板方案

本文覆盖三项需求，并按可交付性排序。前两项涉及 SYSTEM 服务或内核驱动，
不能只靠当前用户态的 shell/core 完成。

## 1. 实时剪贴板（应用层，优先）

### 目标

- 双向实时同步（文本先行，图片/文件后续）
- 多层套娃 A→B→C：A 复制的内容能到 C，且不回环
- 连接时默认以控制端剪贴板覆盖被控端
- 与 RDP 剪贴板共存，不抢占、不重复

### 协议

新增控制消息 `clipboard-sync`：

```json
{"type":"clipboard-sync","seq":123,"origin":"device-id/session-id",
 "hash":"sha256:...","text":"..."}
```

- `origin` 是产生该内容的设备/会话，转发时保持不变
- `seq` 每台设备单调递增；`hash` 用于去重
- 收到后应用到本机剪贴板，并把 `(origin, seq, hash)` 记入最近集合
- 本机剪贴板 watcher 发现变化时，若 hash 在最近集合中则视为回显，丢弃
- 转发给其他已认证会话时保留 `origin`；若目标设备 ID 已在 `origin` 链中，
  或本设备是该 `origin`，直接丢弃（防套娃回环）

### 本机/浏览器

- core 的 `clipboard_service` 增加变更回调（X11 selection / Win32 clipboard /
  NSPasteboard 轮询，200–300 ms + 内容 hash 去抖）
- 连接建立后，客户端在有用户手势时读取剪贴板并发第一条 `clipboard-sync`；
  壳层可通过 `on_permission_request` 自动授予读写权限
- 默认“远端优先”：连接时把控制端内容写给被控端；之后双向
- 设置项：`clipboard_sync`（实时同步）与 `clipboard_remote_priority`（默认开）

### 验收

- 两层/三层套娃复制文本，不出现回环风暴
- 同一内容只广播一次；本机复制后远端能粘贴
- 断开后仍在核心的最近集合中保留 5 秒，避免重连重复

## 2. 虚拟显示器与分辨率

### Windows

创建显示器需要 **IddCx 内核驱动**（如 IddSampleDriver、usbmmidd、Parsec VDD），
应用层无法凭空创建。可交付的部分：

1. 检测当前输出：`EnumDisplayDevices` / DXGI；无输出时标记 `no_display`
2. 若已安装受支持的虚拟显示驱动，调用其控制接口/`ChangeDisplaySettingsEx`
   创建或切换为客户端活动显示器分辨率（`screen.width × height × dpr`）
3. 会话工具栏增加“显示器 / 分辨率”菜单，手动切换；仅列出驱动实际支持的模式
4. 没有驱动时明确提示，不假装成功

建议把驱动厂商的 CLI/COM 接口封装成 `virtual_display_backend`，先支持
Parsec VDD 与 usbmmidd，再考虑自带 IddCx 驱动（单独仓库/签名）。

### Linux/macOS

- X11：`xrandr --setmonitor` 只能声明虚拟 monitor，不能生成可抓取的 CRTC；
  可作为“分辨率切换”支持，虚拟输出需 Xorg 配置或驱动
- Wayland：xdg-desktop-portal ScreenCast 选择已有输出，不能创建
- macOS：无公开 API 创建虚拟显示器，需要 DriverKit/第三方驱动

### 验收

- 无显示器的主机能按控制端活动分辨率得到可抓取画面（有驱动时）
- 工具栏切换分辨率后关键帧重建，输入坐标映射正确
- 无驱动时提示明确，不影响普通远程

## 3. Windows 锁屏 / 控制台 / RDP 会话（SYSTEM 服务）

### 为什么现有架构不行

锁屏、UAC、登录页运行在 **Winlogon 安全桌面**。当前 shell/core 以普通用户
运行，不能 `OpenInputDesktop`/`SetThreadDesktop` 到安全桌面，也不能向它
`SendInput`。RDP 会话与物理控制台也是两个 WTS 会话。

### 架构

新增 `molink-service.exe`：

- LocalSystem 自动启动，注册为 Windows 服务
- 命名管道 + ACL（只允许本机交互用户/SYSTEM），IPC 命令：
  - `list-sessions`：`WTSEnumerateSessions`（Console / RDP / Disconnected）
  - `capture-desktop`：在目标会话打开输入桌面并抓屏（DXGI 或 GDI）
  - `input`：在目标桌面注入
  - `switch-console`：`WTSConnectSession(console, rdp, ...)` 或 `tscon`
  - `kick-rdp`：`WTSLogoffSession` / `tscon rdp /dest:console`
  - `virtual-display`：调用虚拟显示后端
- 服务不保存密码；切换控制台使用 `WTSQueryUserToken`/已登录会话令牌，
  需要凭据时由本机 UI 一次性输入，不落盘
- 锁屏抓屏/输入由服务在安全桌面完成，普通 shell 只负责显示

### 安全

- 管道 ACL = SYSTEM + Administrators + 当前登录用户；校验调用方会话 ID
- 控制台/RDP 切换是高权限操作，必须在本机 UI 二次确认
- 审计日志记录会话切换与踢下线操作，但不记录密码

### 验收

- 锁屏/登录页可看到画面并可输入（SYSTEM 服务）
- 设备被 RDP 占用时，可选择“控制台”，登录后把 RDP 会话踢下线
- 普通用户态无法直接调用切换命令；IPC 鉴权失败有明确错误

## 实施顺序

1. **实时剪贴板**：纯应用层，先文本、再图片/文件；可独立发版
2. **虚拟显示器**：先做检测 + 分辨率菜单 + 已装驱动适配；自带 IddCx 驱动另立项目
3. **Windows 服务**：`molink-service` + 安全桌面抓屏/输入 + 会话切换，单独里程碑

每一步都需要真机回归（Windows 7/10/11、锁屏、RDP、无显示器）。
