# Changelog

## [0.8.1] — 2026-10-25

### Added

- 简洁模式主页：本机卡片 + 连接面板 + 历史/已保存/域组/局域网设备列表；标题栏与设置
  可切换简洁/高级模式，文件与聊天入口会在会话建立后直接打开对应面板
- 本机头像：内置色板或上传图片（自动裁剪压缩为 96×96 WebP），随目录同步并在设备列表
  中显示；设备平台（Windows/macOS/Linux）一并同步
- 内置设备目录：不填写服务器地址也能使用 `https://molink.ssvr.top:28430`；新增远程 ID
  服务器总开关，自定义地址仍可覆盖内置值
- 域组（频道）码：目录为每台设备生成唯一的 9 位随机码，可复制发给其他设备加入同一
  域组；「域组」列表只显示同码设备；码的所有者更新后旧码立即失效，旧成员显示
  「无此域码」
- UI 偏好（界面模式、已保存设备、最近连接）持久化到核心的 `ui.json`，核心端口每次
  启动变化也不会丢失
- Windows 7 x86（32 位）预览客户端：GDI 采集 + SendInput 内核、Win32/WebView2 外壳
  与浏览器 UI 启动器、可选安全桌面服务；提供 Inno Setup 安装器与便携 zip，并新增
  `release-win7.yml`（`win7-x86-v*` 标签）构建工作流

### Changed

- Windows 服务卡片的启动/停止合并为一个切换按钮
- 心跳可同步名称、平台与头像；心跳省略名称时不再把已存名称覆盖为 `device`
- 目录 HTTPS 使用严格证书与主机名校验
- macOS 桌面包启用 OpenSSL 并随 dmg 携带 `libssl`/`libcrypto`，默认 HTTPS 目录在
  Linux、Windows、macOS 三端均可用
- **moLink Server 已拆分到独立仓库**
  [Antruly/molink-server](https://github.com/Antruly/molink-server)，服务端代码、
  打包与发布工作流不再留在本仓库；服务端改用该仓库自己的 `v*` 标签，版本线与
  桌面端解耦

## [0.8.0] — 2026-10-24

### Added

- 二阶段设备目录 `molink-server`：设备注册、心跳、在线状态、按 ID 查询、注销；目录持久化
  `devices.json`，支持注册邀请码、每 IP 注册限流
- 桌面核心接入目录：设置页填写服务端地址/邀请码后自动注册与心跳，分配到的设备 ID 与在线
  状态显示在本机卡片；凭据保存在 `server.json`，重启复用
- 通过设备 ID 连接：连接页新增「地址 / 设备 ID」切换，ID 模式经目录解析地址后直连；
  连接表单不再把设备 ID 作为唯一入口
- 连接信息以计算机名称为主，会话标题、历史记录与提示均优先使用对方电脑名
- 本机卡片支持「名称 / ID / IP」切换：默认在已注册服务端时显示 ID，否则显示计算机名称；
  IP 模式需要开启 IP 连接，未开启时可就地跳转设置，开关即时生效
- 服务端支持原生 HTTPS（`--tls-cert/--tls-key`，TLS 上只宣告 HTTP/1.1），桌面端支持
  `https://` 服务端地址与自定义 CA（设置 → 服务端 → 自定义 CA）
- 安全设置开关改为即时生效，只有计算机名称修改需要点“应用”
- 发布包启用 OpenSSL：Linux deb/rpm 声明 libssl 依赖，AppImage 打包 libssl/libcrypto，
  Windows 安装包/便携版附带 libssl/libcrypto DLL；桌面客户端可使用 HTTPS 服务端
  （macOS 暂未启用，避免 dmg 的 OpenSSL 运行时捆绑）

### Fixed

- 前端 ESLint（React Hooks 规则）通过：消除 effect 内同步 setState、调整语音通话事件
  处理函数的声明顺序

## [0.7.0] — 2026-10-24

### Added

- 全新界面与暗色主题：主页改为“本机状态 + 连接 + 使用提示”布局，信息层级和留白重新整理；
  暗色主题换成蓝灰色调并提高卡片/文字对比度
- 最近连接支持**卡片 / 列表**两种视图，选择会被记住；手动连接改为在当前窗口打开标签页
  （只有会话工具栏的“弹出”或把标签拖出窗口才会新建窗口）
- 会话全屏：只显示远程画面、无内边距和边框，顶部保留可退出全屏的悬浮工具栏；标题栏方形按钮
  改为切换整个应用全屏（与会话全屏是两个独立状态），Esc 在未开启控制时退出全屏
- 会话模式：连接时可选“远程控制 / 仅查看”。远程控制连接后立即启用键鼠，仅查看默认不发送输入
- 会话聊天：远程端与主机端共享同一份聊天记录；主机端右下角提供聊天浮窗和未读提示
- 语音通话（不含视频）：在聊天面板发起，主机端弹窗接听/拒绝；接通后双向 48 kHz 单声道 PCM，
  支持静音与挂断；Windows 主机使用 WASAPI 麦克风采集与扬声器播放
- 连接信息以**计算机名称**为主：附近设备、会话标题和历史记录都优先显示对方电脑名；
  设备 ID 属于第二阶段（服务端注册）能力，不再作为连接入口
- 全屏悬浮工具栏与普通模式行为一致：点击画面或空白处收起为一个小按钮，点击按钮重新展开；
  Esc 先关闭工具栏，再按一次退出全屏
- 文件传输：拖拽或选择文件发送到远程 `Downloads/moLink`；可浏览远程主目录并下载文件；
  分块传输、进度、接收队列
- 文件剪贴板（RDP 风格）：Windows 主机支持 `CF_HDROP` 文件列表。远程资源管理器复制的文件会
  自动出现在“远程剪贴板”中并可接收；本机发送完成的文件可一键复制到远程剪贴板，直接在远程
  资源管理器中粘贴
- 远程声音：Windows 端用 WASAPI 回环采集系统播放的声音（48 kHz 立体声 16-bit PCM），
  会话工具栏与全屏悬浮工具栏可开关并调节音量
- 剪贴板同步开关：会话工具栏与全屏悬浮工具栏可随时关闭/开启文本剪贴板同步
- 检查更新：启动时查询 GitHub 最新 Release；有新版本时在顶部提示，可立即更新（Windows 会下载
  并运行安装包）或手动下载最新安装包

### Fixed

- 弹窗会话的关闭按钮改用 native shell 的关闭动作，不再依赖 WebKit 可能忽略的 `window.close()`，
  点应用内的叉即可退出远程窗口
- Windows 全屏时窗口区域仍按圆角裁剪，全屏后四个角现在是直角
- 全屏悬浮工具栏失去焦点后不会收起；现在和普通模式的快捷工具栏一样会在点击外部时收起

## [0.6.32] — 2026-10-24

### Fixed

- Windows 锁屏/登录页远程输入完全失效：helper 打开输入桌面时只申请了
  `DESKTOP_READOBJECTS|DESKTOP_WRITEOBJECTS|DESKTOP_SWITCHDESKTOP`，`SetThreadDesktop` 会成功，
  但之后每次 `SendInput` 都返回 `ERROR_ACCESS_DENIED`（解锁状态下同样失败）。改用 `GENERIC_ALL`
  打开输入桌面后，模拟按键可唤出登录页，登录页可远程输入密码完成解锁
- 抓屏线程与输入线程各自记录自己的桌面句柄（`thread_local`）；此前共用句柄会让后启动的线程
  误以为已经切换过桌面，从而把输入注入到错误的桌面
- helper 改为在 `winsta0\default` 启动并加 `CREATE_NO_WINDOW`，不再在用户桌面弹出控制台窗口
  并抢走前台焦点

## [0.6.31] — 2026-10-24

### Added

- 断线重连：意外断开后自动重连 3 次（首次立即、之后间隔 1 秒），任一成功则继续且不提示断开；
  三次都失败才在画面中央显示“连接已断开”，并给出 60 秒倒计时，之后每分钟尝试一次、无限重试，
  适合无人值守。对面主动关闭（1000/1008）不重连
- 断开覆盖层提供“立即重连”和“停止重连”

## [0.6.30] — 2026-10-24

### Added

- 极速模式（turbo）：8-bit 黑白灰度编码（`gray8` / `deflate_gray8`），采集与拉帧 5 fps、变化阈值
  更粗，面向极差网络。画质菜单新增“极速”；同屏灰度关键帧实测 ~599 KB（BGRA 通常 2–4 MB）

## [0.6.29] — 2026-10-24

### Fixed

- 流畅画质（RGB565）红蓝通道颠倒：客户端展开 RGB565 时把红色放到了蓝通道位置，导致蓝色画面
  显示成橙色；已修正
- `Ctrl+Alt+Del` 按钮增加提示：Windows 安全界面会忽略模拟输入，需要在本机按键退出

## [0.6.28] — 2026-10-24

### Added

- 瓦片级重传：新增 `request-tiles` 协议消息。客户端丢帧时只请求受影响瓦片，服务端从当前画面裁剪
  这些矩形、压缩成补丁帧（`kPacketFlagTileRefresh`）返回，不再为网络问题触发整帧关键帧
- 每个瓦片记录最后应用的采集时间戳：收到过期标记后再请求前会重新校验，如果该瓦片在标记之后
  已被更新时间的帧覆盖，则直接跳过，不做多余请求
- 服务端回放积压超过 1 秒时，改用瓦片补丁从当前画面恢复，而不是发关键帧
- Windows DXGI / Winlogon 安全桌面 / X11 抓屏都提供当前画面快照以支持瓦片补丁

## [0.6.27] — 2026-10-24

### Changed

- 防积压降帧：客户端一旦丢弃旧帧，就把主机采集帧率降到 8 fps（5 秒无丢帧后恢复预设帧率）；
  自动画质在检测到丢帧时也会降到“流畅”档，减少发包积压

## [0.6.26] — 2026-10-24

### Changed

- 卡顿/积压时优先最新画面：客户端丢弃超过 1 秒的帧并请求关键帧，队列里已有更新帧时跳过旧帧；
  服务端回放积压时不再发送超过 1 秒的旧帧，直接跳到新的关键帧

## [0.6.25] — 2026-10-24

### Fixed

- 新会话一直停在“等待第一帧”，必须先点一下窗口才出画面：WebKitGTK 在窗口无焦点时把页面
  标记为 `hidden`，拉帧循环直接跳过；现在 welcome 到达时立即请求关键帧和帧，`hidden` 时
  仍以 1 秒间隔慢速拉帧

## [0.6.24] — 2026-10-24

### Changed

- 最近连接：**单击只填入表单，双击才建立连接**，避免误触；保存过验证码的条目双击直接连接

## [0.6.23] — 2026-10-24

### Fixed

- 最大化/全屏后整个窗口仍被裁剪在旧的 2560×1640（右侧/底部露出桌面）：根因是窗口的圆角
  shape region 在最大化后没有被替换，**即使 WebView 的 viewport 已经正确也会被裁**。
  现在不再安装圆角遮罩，始终把 shape 设成覆盖整个窗口的矩形

## [0.6.22] — 2026-10-24

### Fixed

- 启动即最大化时，WebView 的 allocation 已经正确但**页面 viewport 仍是旧尺寸**：页面加载完成后
  显式做一次“高度减一像素再恢复”的 resize，强制 WebKit 重新计算 viewport
- 每 500ms 校验 `GdkWindow` 与 WebView 的实际尺寸，不一致就修正，并写
  `/tmp/molink-shell.log` 便于定位

## [0.6.21] — 2026-10-24

### Fixed

- 启动时窗口已被 WM 恢复为最大化（或页面加载前窗口状态已变化）时，WebView 仍停在默认尺寸，
  右侧/底部露出桌面：现在每次 `size-allocate` 和页面加载完成时都用 `GdkWindow` 的实际尺寸
  重新分配 WebView

## [0.6.20] — 2026-10-24

### Fixed

- 直接最大化（默认尺寸 → 最大化）后 WebView 仍停在旧尺寸：这条路径上 GTK 的 allocation 还是旧值，
  改用 `GdkWindow` 的实际宽高重新分配子控件，并在 150ms / 500ms 各重试一次

## [0.6.19] — 2026-10-24

### Fixed

- 最大化/全屏后窗口是 2880×1920，但 WebView 仍停在旧的 2560×1640，右侧和底部露出桌面：
  窗口状态变化后，壳层等 WM 完成 configure，再按窗口客户区显式重新分配 WebView
  （GTK 的 queue_resize 在这条路径上不生效）

## [0.6.18] — 2026-10-24

### Fixed

- 全屏下画面被拉伸、鼠标点击落到错误位置：WebKitGTK 对 canvas 的 `object-fit` 不可靠。
  现在按容器尺寸显式计算画布显示宽高并居中，输入坐标直接使用画布的实际矩形，画面与
  指针映射始终一致（适应/拉伸/1:1 三种模式统一）

## [0.6.17] — 2026-10-24

### Fixed

- 原生壳层全屏错位：不再同时请求 HTML 元素全屏和 GTK 窗口全屏（两者会互相覆盖窗口状态），
  现在只切原生窗口全屏并隐藏应用标题栏/标签栏，切换后重新测量画布尺寸
- “在新窗口打开”不再等待本地 UI 登录状态，直接按 URL 参数建立远程会话，不会再打开本地首页
- 只读/popout 连接不会再覆盖已保存的验证码

## [0.6.16] — 2026-10-24

### Fixed

- 会话被 `closed (1001): idle timeout` 断开：后台标签页把 3 秒 ping 节流到约 1 分钟，超过服务端
  45 秒的 idle 阈值；现在 idle 阈值改为 5 分钟，并在标签页重新可见时立即请求关键帧和帧
- 会话错误栏新增“重新连接”，不用退出页面重开
- 首帧成功率：收到 `request-frame` 时主动唤醒采集线程，避免客户端一直停在“等待第一帧”

## [0.6.15] — 2026-10-24

### Fixed

- Windows MSVC 构建失败：`capture.cpp` 为显示器保活引入 `windows.h` 后，`min`/`max` 宏把
  `std::max` / `std::min` 展开坏（C2589）；定义 `NOMINMAX` 后再包含

## [0.6.14] — 2026-10-24

### Added

- 会话工具栏新增 Ctrl+Alt+Del：核心通过 SYSTEM 服务调用 `SendSAS`；安装/升级服务时把
  `SoftwareSASGeneration` 设为 1（仅允许服务生成 SAS）
- 连接页新增“发送远程请求”（无需验证码）；等待被控端授权时显示 60 秒倒计时，可随时取消
- 历史记录：带已保存验证码的条目点击即直连，另有“填入表单”按钮
- “在本机保存此授权码”默认勾选；从历史填入时保持勾选
- 远程会话期间保持显示器常亮，并唤醒已息屏的显示器（`SetThreadExecutionState` +
  `SC_MONITORPOWER`）

### Fixed

- 验证码输错后保留输入内容，可直接修正后重试
- 服务安装时启用 `SoftwareSASGeneration`，Ctrl+Alt+Del 才能生效

### Notes

- 没有物理显示器时仍需要 IddCx 虚拟显示驱动；没有驱动时核心回退到合成帧并在 UI 标注，
  不会假装创建了显示器

## [0.6.13] — 2026-10-24

### Fixed

- 锁屏/登录页画面一直出不来：helper 写完回复后立即 `DisconnectNamedPipe`，消息型管道里尚未被客户端读取的数据会被丢弃
  （`info` 读到 0 字节），核心因此回退到 DXGI。现在写入后调用 `FlushFileBuffers`，等客户端读完再断开；helper 的采集/输入
  管道和 service 控制管道都做了同样处理
- Winlogon 桌面抓屏失败：安全桌面不支持 `CAPTUREBLT`，`BitBlt` 返回错误；改为失败后用 `SRCCOPY` 重试
- 桌面切换后抓屏失败：GDI 屏幕 DC 绑定在线程首次使用的桌面上，Winlogon ↔ Default 切换后原线程 `BitBlt` 会
  `ACCESS_DENIED`；失败时在新线程绑定当前输入桌面重试
- helper 日志补充启动时的 window station/线程桌面、输入桌面、桌面切换和抓屏失败原因（失败日志 10 秒节流）

## [0.6.12] — 2026-10-24

### Fixed

- Windows 服务在 moLink 启动约 1 秒后自动停止：`app.start()` 启动事件循环后立即返回，紧随其后的
  `stop_session_service` 被当成启动路径执行；现在只在真正退出时停止服务
- 覆盖安装后服务不再自动启动：旧版安装把服务 DACL 限制为启动/停止/查询，`--install` 以
  `SERVICE_ALL_ACCESS` / `SERVICE_CHANGE_CONFIG` 打开服务会被拒绝并返回 1；升级安装改用最小权限打开，
  新建服务时给管理员完全控制
- 服务安装/启动失败时输出具体阶段和 Win32 错误码，便于定位

### Added

- IPv6：前端与桌面壳拼 URL 时为 IPv6 字面量加方括号（`client.ts`、`api.ts`、Linux/Windows 壳层）
- `--host ::` 与 `0.0.0.0` 一样视为通配地址，不再要求 `--allow-remote`
- 桌面壳（Linux/Windows）新增 `--host`，转发给核心并用于窗口 URL

## [0.6.11] — 2026-10-24

### Added

- 主页显示后台服务状态，支持安装、启动、停止、重启，并说明安装后的额外功能（锁屏/登录页远程、Windows 会话切换）
- 持久化用户期望的服务状态 `service_desired_running`：手动停止后下次启动保持关闭；启动后下次自动启动；覆盖安装不会强行改变用户选择

### Fixed

- 覆盖安装时服务正在运行，`CopyFile` 失败导致 ProgramData 里仍是旧服务；`--install` 现在先停止（StopPending 时终止）旧服务，再复制并启动新二进制；`--uninstall` 同样处理
- 0.6.10 的服务停止卡死、helper 权限/桌面、日志和 0.6.9 的独立输入管道一并包含

## [0.6.10] — 2026-10-24

### Fixed

- 服务停止卡在 StopPending：`ConnectNamedPipe` 永久阻塞导致线程无法 join，之后 StartService 再也起不来；停止时主动连接两个管道并发送 quit 唤醒
- 服务启动 helper 前启用 `SeTcbPrivilege` / `SeAssignPrimaryTokenPrivilege` / `SeIncreaseQuotaPrivilege`，并优先尝试 `winsta0\Winlogon`（登录页）再回退 `winsta0\default`
- 服务/helper 写日志到 `%ProgramData%\moLink\service\{service,helper}.log`，便于定位锁屏问题
- 0.6.9 的独立输入管道（`molink-secure-input`）一并包含

## [0.6.9] — 2026-10-24

### Fixed

- 安全桌面输入改用独立命名管道 `molink-secure-input`：之前输入命令与 8 MB 帧共用一个管道，输入经常排在帧传输后面而表现为“画面能看但控制不了”
- helper 同时运行采集与输入两个 pipe server；输入线程刷新自己的桌面句柄

## [0.6.8] — 2026-10-23

### Added

- 设置 → 后台服务：查看安装/运行状态，手动启动、停止、重启、卸载
- `/api/service/restart`；核心启动时对 StartService 增加重试，避免上一次停止尚未完成导致服务保持停止

### Notes

- 0.6.7 的安全输入回退修复与 8 fps/缓冲复用已包含；锁屏输入仍需真机验证

## [0.6.7] — 2026-10-23

### Fixed

- 安全桌面输入：初始化时不再要求 info 探测成功，避免与首帧传输抢管道失败后回退到 SendInput（SendInput 无法进入 Winlogon，表现为画面可见但无法控制）
- 安全桌面采集帧率上限 8 fps，并复用帧缓冲，降低 GDI + 大块管道传输造成的卡顿

## [0.6.6] — 2026-10-23

### Fixed

- Windows 构建：安全桌面采集/输入源未定义 `NOMINMAX`，`windows.h` 的 `min`/`max` 宏破坏了 `std::min`/`std::max`；修复后 0.6.5 的锁屏接入可以正常编译

## [0.6.5] — 2026-10-23

### Added

- Windows 锁屏/登录页远程（实验性）：服务以 SYSTEM 令牌在活动控制台会话启动 helper，核心在控制台会话优先使用安全桌面采集，并把输入转发到 helper
- helper 支持 Winlogon/UAC 桌面切换、滚轮、释放按键；输入坐标按 helper 当前桌面分辨率刷新，适配锁屏分辨率变化
- 安全桌面采集走同一套瓦片/画质/压缩管线，客户端无需新协议

### Notes

- 仅当核心运行在物理控制台会话时启用；通过 RDP 启动的 moLink 仍使用 DXGI 捕获该 RDP 会话
- 需要在 Windows 真机锁屏/登录页验证；服务安装与提升权限按 0.6.4 的流程

## [0.6.4] — 2026-10-22

### Added

- Windows 后台服务接入：便携版启动时弹窗询问是否安装；安装包新增「安装后台服务」任务（默认勾选）
- 服务安装到 `%ProgramData%\moLink\service`，按需启动；服务 DACL 允许本机用户启动/停止，安装/卸载仍需要管理员
- 核心启动时自动启动服务、退出时停止；锁屏/登录页远程与 Windows 会话切换的基础设施
- 本机 API：`/api/service/status|install|start|stop|uninstall`（仅回环）

## [0.6.3] — 2026-10-22

### Fixed

- 新客户端没有基准图像时，服务端只检查最新帧是否关键帧；若最新帧已经变成增量帧，会一直发送空帧，永远等不到关键帧。现在向采集历史回溯最近的 keyframe 并回放（Windows 真机流畅档首帧复现）
- 回放上限提升到 120 帧，与采集历史一致

## [0.6.2] — 2026-10-22

### Fixed

- 免密审批等待期间不再触发 45 秒 idle timeout；批准时刷新 `last_seen`
- supervisor 关闭超时会话前先从注册表移除，避免 libuvcpp 复用传输对象时把新连接误杀；这是在 Windows 10 真机首帧测试中定位的
- Windows 真机验证：0.6.1 在 1920×1080、DXGI 采集下首帧关键帧 0.2 秒到达（813 KB，9 瓦片）

## [0.6.1] — 2026-10-22

### Fixed

- Windows core build: `api.cpp` included `<windows.h>` before winsock2, causing `sockaddr`/`winsock2` redefinition errors; use `WIN32_LEAN_AND_MEAN` and include winsock2 first.

### Added

- Experimental secure-desktop helper source (`shell/windows/helper`) and `secure-start`/`secure-stop` service commands; CI compiles both, not wired to the core yet.

## [0.6.0] — 2026-10-22

### Added

- 实时剪贴板（文本）：核心转发、被控端剪贴板 watcher、内容 hash 去重、套娃链防回环；连接时默认以控制端剪贴板为准
- 显示器分辨率切换：会话工具栏选择分辨率，Windows 通过 `ChangeDisplaySettingsEx` 应用
- 实验性 Windows 会话服务 `molink-service`：WTS 会话枚举、`tscon` 切回控制台、踢 RDP；设置页「Windows 会话」列表；控制类操作要求管理员权限
- Windows 会话/虚拟显示器/剪贴板实施文档

### Experimental / limitations

- 锁屏、UAC、Winlogon 安全桌面的抓屏与输入**尚未实现**；`molink-service` 未在 Windows 真机验证
- 创建虚拟显示器仍需 IddCx 内核驱动；当前只支持在已有输出上切换分辨率
- 实时剪贴板暂支持文本；图片/文件与 RDP 真机共存待验证

## [0.5.1] — 2026-10-22

### Added

- 免密远程审批：没有验证码的客户端可发起 `request-access`，被控端本机弹窗允许/拒绝；设置项默认开启，可关闭
- 快捷工具条改为可拖动的悬浮方块：点击在顶部展开，失焦/点击外部自动收起，收起后回到拖动位置
- 连接状态、分辨率与 codec 水印合并为一个开关（工具条上的状态按钮），默认隐藏
- 移动端虚拟鼠标/键盘悬浮条支持拖动，松手自动吸附到左右边缘

### Changed

- 画质默认改为“自适应”（仍可在会话工具条或偏好设置中固定档位）

### Fixed

- Windows 7：壳层直接导入 `GetDpiForWindow`（Win10 1607+）导致启动报缺少入口点；改为运行时解析并回退到系统 DPI，导入表回到 Win7 级别
- Ubuntu 最大化/全屏：窗口圆角 shape 在状态切换时未重置，导致画面被裁剪、标题按钮错位且无法恢复；进入最大化/全屏时清除 shape，并重新布局 WebView
- 全屏按钮同时请求浏览器元素全屏和原生窗口全屏，隐藏应用顶栏/标签栏，获得真正的全屏画面

## [0.5.0] — 2026-10-22

### Added

- 画质预设：流畅 / 平衡 / 清晰 / 自适应，默认平衡；会话快捷工具条与偏好设置均可切换并持久化
- 流畅档使用有损瓦片：RGB565（16 位色）在压缩前就把每像素 4 字节降到 2 字节，并固定 zlib level 1；实测 2880×1920 桌面关键帧约 599 KB → 312 KB
- 新增协议 codec 6–9：`rgb24`、`rgb565`、`deflate_rgb24`、`deflate_rgb565`；浏览器解压后按位扩展，不引入 JPEG/WebP 的异步解码路径
- 清晰档使用 zlib 3/6/9 并把变化阈值收紧到最多 1；平衡档保持原有无损行为
- 自适应档根据 RTT、含空帧的拉帧完成率和吞吐量切换预设，带两次采样确认与 3 秒迟滞
- 画质预设同时调整采集帧率与拉帧间隔（流畅 30 fps/33 ms、平衡 20 fps/50 ms、清晰 15 fps/66 ms）
- 会话画面显示模式（适应 / 1:1 / 拉伸）、快捷操作条与全屏切换
- `welcome.tiles.quality` 与 `quality` 控制消息回执，报告当前生效的预设与瓦片格式
- 关键帧瓦片并行压缩（最多 8 线程），2880×1920 关键帧编码从约 50 ms 降到约 18 ms；`MOLINK_SERIAL_KEYFRAME=1` 可退回单线程
- 前端瓦片扩展改为 32 位整像素写入 + RGB565 查表，关键帧解码扩展耗时约减半
- `tools/bench_quality.py`：对运行中的核心测量三档预设的关键帧大小、吞吐、FPS 和帧间隔，并给出 PASS/FAIL

### Changed

- `quality` 控制消息改为接受 `mode`（`smooth`/`balanced`/`quality`），`fps` 变为可选；只带 `fps` 的旧客户端行为保持不变
- 有损格式压缩不划算时回退为原始 RGB24/RGB565，而不是 BGRA

### Fixed

- 客户端回应服务端心跳的 `pong` 被当成未知控制消息，导致会话里周期性出现 `unsupported control type: pong` 错误
- `authenticated` 标志在会话线程写、supervisor 线程读时缺少同步，偶发把已认证会话误判为未认证并在 5 秒后以 `hello timeout` 关闭
- X11 采集退出时先 `dlclose` 扩展库再 `XCloseDisplay`，Xlib 的清理钩子跳进已卸载模块，导致每次 SIGTERM 退出都段错误；现在先销毁 damage 对象并关闭 display，再卸载库

## [0.4.2] — 2026-10-22

### Fixed

- Windows Inno Setup 安装包自带 `ChineseSimplified.isl`，不再依赖 Inno Setup 安装目录里的语言文件；修复 Windows Release 构建中 `Couldn't open include file ...ChineseSimplified.isl` 的失败

## [0.4.1] — 2026-10-22

### Added

- 移动端 Web 虚拟鼠标：整块画面作为触摸板，滑动移动远端光标，点击左键，长按右键，双指上下/左右滚动；虚拟鼠标模式下显示远端光标
- 移动端虚拟键盘：触摸设备显示“虚拟键盘”按钮；支持带组合输入（IME）的文本注入
- `text` 输入事件与输入后端：Windows `KEYEVENTF_UNICODE`、macOS `CGEventKeyboardSetUnicodeString`、X11 Unicode keysym 临时映射
- PC 端不显示虚拟鼠标 / 虚拟键盘按钮

### Fixed

- Windows 资源编译器把图标路径中的 `\a`、`\m` 当成转义字符，导致 `RC2135: file not found`；生成的 `.rc` 改用正斜杠路径

## [0.4.0] — 2026-10-22

### Added

- 自有端口段，避开 RustDesk 的 21115–21119 与 TeamViewer 的 5938：`28418` 网页/API/WS、`28419` HTTP/3、`28420` QUIC 数据、`28421` TCP 数据、`28422` UDP 数据；全部可通过命令行或配置文件修改并持久化
- `--http3-port` / `--quic-port` / `--tcp-data-port` / `--udp-port` 命令行参数；`/api/network` 接口与 `/api/state` 中的端口 / `restart_required` 字段
- `GET /api/transports` 传输协商表，以及 `ready` / WebSocket `welcome` 中的 `transports` 与 `transport_ports`；客户端按 `preferred` 顺序选择第一个可用传输
- Web 端保持 WebSocket 基线；HTTP/3、QUIC、TCP/UDP 直连数据已预留端口与协商字段，数据面接入前统一报 `available=false`
- 瓦片按行合并成水平条带，关键帧从每行 N 个瓦片降为 1 个；减少表项与 zlib 流数量
- zlib 压缩级别自适应：关键帧/大突发 level 1，少量小瓦片 level 3/6，兼顾延迟与压缩率
- 前端离屏 canvas 双缓冲，整帧完成后一次上屏；分辨率变化等待关键帧，避免半帧/黑块
- `capture-fps` 控制消息与客户端自适应帧率：RTT/带宽过高时降到 10/5 fps，恢复后回到 15 fps；隐藏标签页停止拉帧
- 按 CPU 线程数自动选择瓦片粒度（128/256/512 px），`--tile-size` 可覆盖
- 模糊变化阈值（默认 4/255，`--change-threshold 0` 可关闭）：忽略抗锯齿与噪声，只重传真正可见的变化
- `--tile-size` / `--change-threshold` / `--capture-fps` 作为质量与速度的底层调节项；界面预设与有损瓦片 codec 留待后续
- 新增 `session_transport` 抽象层与 `ws_transport` 实现；`session_controller`（原 `ws_session`）不再直接持有 WebSocket 连接，会话注册表和回收线程只依赖传输接口，为 TCP/QUIC 数据通道铺路
- 桌面壳单实例：同一用户会话重复启动会聚焦已有窗口，不再启动第二个核心；Linux/macOS 使用 Unix socket，Windows 使用命名互斥量，macOS 另设 `LSMultipleInstancesProhibited`
- 安全设置与默认收紧：「允许局域网 / IP 访问」「允许浏览器客户端」「局域网发现」三个开关默认关闭，保存在 0600 配置文件；核心监听 `0.0.0.0`，但关闭时非回环 TCP 在 HTTP 解析前立即关闭
- 局域网访问开关热生效，无需重启；关闭时立即断开已有远程 WebSocket 会话（显式 `--host 127.0.0.1` 仍为真正仅本机）
- 暗色主题改为中性深灰底 + 青色强调，去掉原来的墨绿色泛光；亮色主题保持不变
- 授权码分层：长期码（设置页配置，8+ 位且含字母和数字）、单次码、多次码（支持自定义值和次数，用完自动换新）；登录返回会话 token，后续 API 不再接受授权码作为 Bearer
- UDP `28423` 组播发现（组 `239.255.42.99`），beacon 只含设备 ID/名称/端口；主页新增“附近设备”，点击填入地址和端口
- 设备 ID 输入自动补横杠（`ABC-DEF-GHJ`）
- 主页移除右下角重复的语言/皮肤设置，安全设置集中到右上角设置面板
- 壳层身份 token：`--print-ready` 输出 `client_token`，默认关闭普通浏览器访问；壳层通过 `client=` / `X-MoLink-Client` 打开 UI，WebSocket 不受影响
- 连接表单支持“记住授权码”（仅本机浏览器存储）；附近设备双击时若已有保存的授权码会自动连接
- 被控端多显示器选择：默认 index 0（主显示器），会话工具栏可切换；X11 用 XRandR、Windows 用 DXGI 输出枚举、macOS 用 CGGetActiveDisplayList；输入坐标按所选显示器映射
- Windows 桌面图标与任务栏图标：图标资源编译进 `moLink.exe`，窗口类、`WM_SETICON`、托盘和快捷键共用；设置 `AppUserModelID` 避免任务栏分组使用默认图标
- 远程验证按 IP 阶梯封禁：每轮 5 次失败锁定 30 秒 / 2 分钟 / 10 分钟 / 1 小时 / 6 小时，第 6 轮永久封禁；`/api/login`、其他 `/api/*` 的错误 Bearer 令牌与 WebSocket `hello` 共用额度，封禁名单持久化到 `banned_ips`
- Windows 安装器（默认勾选）添加专用/域网络防火墙规则，放行 `molink-core.exe`

### Changed

- 默认监听 `0.0.0.0`，其他设备可直接访问登录页；`--host 127.0.0.1` 可恢复仅本机监听。显式指定其他非回环地址仍需 `--allow-remote`
- Windows 安装包从 NSIS 换成 Inno Setup 6：现代向导、桌面/开始菜单快捷键显式使用应用图标；为添加防火墙规则而请求管理员权限。Release 继续提供免安装单文件 `.exe`
- 桌面壳不再以 `--port 0` 启动核心，改为使用固定端口或用户保存的端口；Windows 重复启动会聚焦已运行的窗口

### Fixed

- Windows 安装后的桌面快捷方式、安装包自身和任务栏按钮显示白图标/默认图标的问题

## [0.3.0] — 2026-10-21

### Added

- 本机（回环）访问免验证：页面通过 `/api/device` 自动取得本机令牌
- 非本机访问核心 UI 必须输入验证码；验证后可进入主页并操作该设备
- 远程验证后可一键“控制这台设备”（核心所在主机），无需再次验证
- 空闲自动锁定：默认 60 秒，可 `--lock-timeout` 或偏好设置修改并持久化
- 锁定仅在“无远程会话且无操作”时触发；多窗口会话数通过 `/api/ui-state` 汇总
- `/api/state`、`/api/login`、`/api/activity`、`/api/ui-state`、`/api/lock-timeout`
- 登录失败限速（5 次失败后暂停 30 秒）
- 本机自连防护：设备页隐藏“连接本机”，表单拒绝回环地址 + 本机端口

### Changed

- 壳层 URL 不再携带验证码；本机令牌由回环 `/api/device` 获取
- 远程模式下新连接使用 Tab（避免独立窗口二次登录）；标签拖出仍限本机
- 锁定状态下 WebSocket 握手被拒绝（`1008 locked`），HTTP API 返回 `423`

### Fixed

- 设备页锁定时长下拉框对非预设值显示错误（现在动态包含当前值）

## [0.2.0] — 2026-10-21

### Added

**界面**
- 设备页：本机设备 ID 与验证码（默认隐藏、可复制），能力状态，最近连接
- 多会话：首次连接在独立窗口打开，后续连接进入 Tab；标签可拖出成独立窗口
- 亮色 / 暗色主题，跟随系统初始值并记住选择
- 连接质量水印，默认关闭，可在偏好设置或会话工具栏开启
- 无边框圆角窗口 + 自绘标题栏（Linux/Windows），边缘拖拽缩放
- 关闭窗口收进托盘（Linux GtkStatusIcon / Windows Shell_NotifyIcon）；macOS 保留在 Dock
- 新应用图标：青绿→天蓝渐变 + 白色 “m”，同时用于窗口、托盘与安装包

**后台**
- 持久化设备身份：设备 ID 与验证码保存在配置目录，验证码不再每次启动变化
- `/api/device`（仅回环地址）向本机界面提供设备信息
- 修复系统分配端口时 `/api/device` 与 `/api/info` 报告 `port=0` 的问题

**壳层**
- 统一 `--url` 模式：独立会话窗口复用同一个壳层进程，无需再启动核心
- Linux：WebKit `create` 信号派生新窗口；GTK 窗口消息处理
- Windows：WebView2 `NewWindowRequested`、WebMessageReceived、托盘、圆角区域
- macOS：`WKUIDelegate` 新窗口、脚本消息、关闭隐藏、`--url` 模式

**打包**
- Windows 免安装单文件 `.exe`（NSIS 静默解压到临时目录后运行）
- Linux RPM（`rpmbuild`，与 `.deb` 相同布局）
- macOS `.app` 使用 `icon.icns`，dmg 包含应用图标
- AppImage 启动前检查缺失依赖并给出可读提示；补齐 `.DirIcon`

### Fixed
- 标签页弹窗重复、首次连接与后续连接的窗口/Tab 规则
- 无边框窗口无法拖动/缩放（原生消息 + 边缘热区）

### Known gaps
- Wayland/PipeWire 尚未支持，会回退合成帧
- H.264/AV1 未接入
- Windows/macOS 后端由 CI 编译，真机回归仍待进行
- Windows/macOS 安装包未签名；macOS 为 ad-hoc 签名

## [0.1.0] — 2026-10-21

### Added

**molink-core（C++20 / libuvcpp）**
- HTTP/WS 后台：`/healthz`、`/api/health`、`/api/info`、`/api/sessions`、`/ws/session`
- 令牌鉴权（Bearer / `X-MoLink-Token`）、默认仅回环地址、CORS 白名单
- MOLK 协议 v1：32 字节包头、瓦片帧体、关键帧/增量帧、有界帧历史与补发
- Linux X11 抓屏：XShm 全量抓取、XDamage 受损矩形、瓦片 diff、30 秒全量刷新
- Windows 抓屏：DXGI Desktop Duplication、瓦片 diff、光标位图（图标遮罩合成）
- macOS 抓屏：CoreGraphics 主屏抓取、瓦片 diff、屏幕录制权限提示
- 无会话时暂停抓屏；会话断开释放按键与鼠标按钮
- 瓦片 zlib 压缩（codec 5 `deflate_bgra`），不划算时逐帧回退原始 BGRA
- 输入注入：Linux XTEST、Windows SendInput、macOS CGEvent；指针、按键、滚轮、组合键
- XFixes 光标位图与位置（规格化坐标 + 热点）
- 文本剪贴板双向同步：Linux X11 selection（INCR）、Windows CF_UNICODETEXT、macOS NSPasteboard
- 父进程监视（`--parent-pid`）、`--print-ready` 就绪行
- `--no-capture` / `--no-input` / `--no-compress` / `--no-clipboard` 开关

**frontend（React 19 + TypeScript）**
- 连接页 / 会话页，中英双语，深色主题
- MOLK 解码、BGRA 与 deflate 瓦片渲染、帧序保证
- 控制模式（手动开启）、归一化坐标映射、组合键与滚轮
- 远端光标覆盖层（控制模式下由本地光标取代）
- 剪贴板复制/发送按钮
- FPS / 延迟 / 带宽 / 帧数统计

**shell / 打包 / 仓库**
- Linux 壳：GTK3 + WebKitGTK，启动核心、解析 ready 行、加载核心托管的前端、
  退出回收、F11 全屏、`--smoke-test`
- Windows 壳：Win32 + WebView2，运行时加载 WebView2Loader.dll，同样的
  核心监管与 `--smoke-test`
- macOS 壳：Cocoa + WKWebView，.app 布局（Contents/MacOS + Resources/web）
- 核心静态托管前端（`--web-root`），Web 版与桌面壳共用同一入口
- Linux `.deb`：`/opt/molink/{bin,lib,web}`，核心 `$ORIGIN/../lib`
- Linux `.AppImage`：AppDir 布局，依赖系统 WebKitGTK
- Windows NSIS 安装器、macOS `.dmg` 打包脚本
- CI：前端、协议、三个平台壳层编译
- 发布工作流：Linux/Windows/macOS 并行构建并附加到同一 Release
- MIT LICENSE、协议文档、发布检查清单、上游问题记录

### Verified

- 36 项协议编解码断言
- Xephyr 嵌套 X server 中的输入注入、光标与剪贴板端到端验证
- 压缩路径像素级比对（画布中心像素与源窗口一致）
- 1280×800 / 2880×1920 真实桌面抓屏；活跃会话约 7–24% 单核、7.4 Mbps（zlib）
- Linux 桌面壳在 Xephyr 中通过冒烟测试（`SHELL_SMOKE_OK title=moLink`）
- 解包 `.deb`、运行 `.AppImage` 后冒烟测试同样通过
- Windows 壳用 MinGW-w64 交叉编译通过（链接成功并产出 WebView2Loader.dll）
- `uvp_verify`：健康检查、鉴权、根路由断言全部通过

### Known gaps

- Windows 与 macOS 后端尚未在真实平台运行验证（Windows 通过 MinGW 编译，macOS 待 CI 编译）
- macOS 未提供远端光标位图（仅本地光标），且需要屏幕录制与辅助功能权限
- macOS 包为 ad-hoc 签名，正式分发需要 Developer ID 与公证
- Wayland / PipeWire 门户抓屏未实现
- H.264 / AV1 + WebCodecs 未接入
- UDP 直连、WebRTC、P2P、`moLink Server`、移动端属于二阶段
