# 与 RustDesk 的功能对照

对照基准：RustDesk `51f9439`（浅克隆，只读参考）。
我方：0.12.19 之后（#70 / #71 / #73 / #74 / #75 / #77 / #78 已合入）。

这份文档只记录**有代码证据**的结论；没有验证过的项目标「未验证」，不猜。

## 一、结论

**强于 RustDesk 的地方**

| 项 | 证据 |
|---|---|
| 脏区网格（128 px cell + tile 时钟） | `backend/src/capture/capture.cpp` `update_cell_clock`；RustDesk 整屏编码（`src/server/video_service.rs:818-886`） |
| 输入后立即采集 | `capture.h` `pending_input_us()` + `wait_until()`；RustDesk 只按帧间隔取帧 |
| 嵌套 RDP 剪贴板识别 | `windows_clipboard.cpp` 识别 `mstsc.exe` / `TscShellContainerClass` / RDP 临时路径；RustDesk 无专门处理 |
| 平台原生虚拟文件 | Windows OLE `IDataObject`（`windows_virtual_clipboard.cpp`）、Linux FUSE（`x11_clipboard_fuse.cpp`）、`IStream` 按 256 KiB 拉取 |
| 输入错误可诊断 | `bad_input` 带具体 code + 5 s 限流 + 诊断日志；RustDesk 只写日志 |
| 路径穿越 / Windows 保留名校验 | `file_transfer.cpp` `parse_relative_path` + `is_reserved_name`；RustDesk 只查 `..` |

**仍落后于 RustDesk 的地方**

| 项 | 现状 | RustDesk |
|---|---|---|
| Wayland | 无输入注入、无剪贴板后端（README 已注明回退合成帧） | uinput + RemoteDesktop portal，`wayland-data-control` 剪贴板 |
| 硬件编码 | 只有 NVENC | VAAPI / VideoToolbox / Media Foundation |
| 前端解码 | 主线程 WebCodecs + canvas2D 双拷贝 | Rust 侧解码直写 GPU 纹理 |
| 端到端加密 | 只有 relay 外层 TLS + 目录下发证书 pin | NaCl box 交换会话密钥，relay 无法解密 |
| 远程打印 / 端口转发 / 白板 / 会话录像 | 无 | 有 |

## 二、逐项对照

### 采集与编码

| 能力 | 我方 | RustDesk | 判定 |
|---|---|---|---|
| X11 采集 | XDamage + XShm | `libs/scrap` X11 | 对等 |
| Windows 采集 | DXGI Desktop Duplication + GDI 回退 | DXGI + GDI 回退 | 对等（HDR tonemap 仍缺） |
| 脏区 | 128 px cell 网格 | 整帧 | **我方更好** |
| 帧节奏 | 绝对 deadline 网格（#71） | `sleep(spf - elapsed)` | 对等 |
| 输入触发采集 | 有（30 ms 下限） | 无 | **我方更好** |
| 硬件编码 | NVENC | NVENC / VAAPI / VT / MF | 落后 |
| 码率原地调节 | `set_bitrate`（#71） | `set_quality` | 对等 |
| 关键帧请求 | 会话 1 s 节流 + PLI 接入 | GOP 极大 + 解码失败重启 | **我方更稳** |
| 多显示器热插拔 | 仅枚举一次 | 每秒检测并广播 | 落后 |
| 光标 | 独立 shape + 位置，100 ms 采样 | 独立通道，33 ms | 落后（采样率） |

### 传输

| 能力 | 我方 | RustDesk | 判定 |
|---|---|---|---|
| 视频队列 | **丢最旧 delta**（#71） | 队列深度 1 | 对等 |
| TCP 背压 | 只丢视频，不断链（#71） | 阻塞采集 | 对等 |
| 自适应 | 2 窗口 + 3 s 驻留 + 排队时延判据（#71） | 扣除 RTT 的排队时延 | 对等 |
| 直连 / 中继 | ICE + TCP 回退 | 同 | 对等 |
| E2E 加密 | 无 | NaCl box | 落后 |

### 输入

| 能力 | 我方 | RustDesk | 判定 |
|---|---|---|---|
| 键位映射 | **物理位**（evdev+8 / scan code，#73） | Map / Translate / Legacy 三模式 | 对等（模式切换仍缺） |
| 桌面 IME | 有（#73） | Unicode / 序列输入 | 对等 |
| 修饰键对账 | 会话级 held + 10 s 超时释放（#75） | 每条事件修正 + 360 s 上限 | 对等 |
| CapsLock/NumLock 同步 | 未上线 | 事件带锁状态 | 落后 |
| 小键盘 NumLock | 强制出数字（语义不对） | 跟随客户端 | 落后 |
| 滚轮 | float + 累积（#75） | WHEEL_DELTA 归一 | 对等 |
| 侧键 | 支持（#75） | 支持 | 对等 |
| 相对鼠标 / 指针锁定 | 仅虚拟触控板 | 完整 | 落后 |
| 多显示器坐标 | 每次注入前重设矩形（#75） | 每次 remap | 对等 |
| Wayland 注入 | 无 | uinput / portal | 落后 |

### 剪贴板

| 能力 | 我方 | RustDesk | 判定 |
|---|---|---|---|
| 文本 | 双向（#74 按复制事件判定） | 双向 + owner 标记 | 对等 |
| 图片 | 本批实现中（6a） | 支持 PNG/SVG | 追赶中 |
| HTML / RTF | 无 | 支持 | 落后 |
| 文件（Windows） | OLE `FileContents` + `CF_HDROP` | `FILEDESCRIPTORW` + `FILECONTENTS` | 对等 |
| 文件（Linux） | `text/uri-list` + FUSE 挂载 | `text/uri-list` | 对等 |
| 文件（macOS） | 真实路径（无虚拟） | `NSPasteboardItemDataProvider` | 落后 |
| 回显抑制 | owner + 序列号 + 内容窗口 | owner 格式位 | 对等 |
| 与其它远控共存 | owner 判定 + 采纳外部变化（#74） | owner 标记 | 对等 |
| 多会话路由 | 前台租约 + 最近输入 + 显式广播开关（#74） | 显式 opt-in | 对等 |
| X11 PRIMARY | 无 | 有 | 落后 |

### 文件传输

| 能力 | 我方 | RustDesk | 判定 |
|---|---|---|---|
| 断点续传 | 有（#78） | 有（`.download` + digest） | 对等 |
| 丢块恢复 | ack 信用窗口（#70） | per-file digest + confirm | 对等 |
| 临时文件隔离 | 每实例唯一名（#70） | 每 job 独立 | 对等 |
| 路径安全 | 穿越/保留名/symlink（+ 可选沙箱 #78） | 仅 `..` 与 symlink | **我方更严** |
| 大文件 | 8 GiB 上限 | 无上限 | 落后（可配置化） |
| 中文文件名（Windows） | UTF-8 统一（#70） | Rust 原生 | 对等 |
| 目录结构 | 文件夹被展平、空目录丢失 | 保留 FILEDESCRIPTOR 属性 | 落后 |
| 0 字节文件 | 前端过滤（bug） | 正常传输 | 落后 |
| 8 文件上限 | 有 | 无 | 落后 |

### 安全

| 能力 | 我方 | RustDesk | 判定 |
|---|---|---|---|
| 验证码 | 6 位、单次/多次、20 天轮换 | 6 位 | 对等 |
| 失败限流 | 5 次起 30s→21600s，第 6 轮永久 | 同级别 | 对等 |
| 2FA | TOTP（#77） | TOTP | 对等 |
| 按功能授权 | 10 项能力位 + 只读/协助预设（#77） | Keyboard/Clipboard/Audio/File/... | 对等 |
| 连接上限 | 全局 64 / 单 IP 16（#77） | 64 / 16 | 对等 |
| IP 白名单 | 无 | 有 | 落后 |
| E2E | 无 | 有 | 落后 |

### 音频与其它

| 能力 | 我方 | RustDesk | 判定 |
|---|---|---|---|
| 音频编码 | 裸 PCM 1.5 Mbps | Opus ~64 kbps | 落后 |
| 静音门 | 无 | 有 | 落后 |
| 语音通话 | 有（裸 PCM，无 AEC） | 有 | 对等 |
| 聊天 | 有 | 有 | 对等 |
| 远程打印 / 端口转发 / 白板 / 录像 | 无 | 有 | 落后 |

## 三、本轮修复清单

见 `CHANGELOG.md` 与对应 PR：

- #70 文件上传 ack 节奏、临时文件唯一名、Windows UTF-8
- #71 egress 新帧优先、TCP 不断链、QoS 驻留、X11 绝对排帧、编码器原地改码率
- #73 物理键位注入、桌面浏览器 IME
- #74 重复复制必同步、前台租约、广播默认关、不外泄主机路径
- #75 会话级按键释放、批量不吞 key-up、滚轮精度、侧键、分辨率后坐标
- #77 两步验证、按功能授权、连接上限
- #78 断点续传、可选写入沙箱

## 四、剩余差距的处理顺序

1. 剪贴板图片（进行中） → macOS 虚拟文件 → 多格式（HTML/RTF）
2. 音频 Opus + 静音门
3. 多显示器热插拔 + 主屏判定
4. 前端 Worker + WebGL、VAAPI / VideoToolbox
5. Wayland（uinput 或 portal）与运行时检测提示
6. IP 白名单、E2E 会话密钥
7. 远程打印 / 端口转发（按需）
