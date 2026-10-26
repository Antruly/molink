# Changelog

## [0.9.27] — 2026-10-26

### Changed

- AV1 标识移到始终可见的“帧率/延迟/带宽”统计栏旁，绿色 `AV1` 徽标，悬停显示
  wire → decoded 字节数；持续 10 秒没有 AV1 补丁才消失（原来 5 秒，且在可折叠
  工具栏/底部状态栏里，容易看不到）。

## [0.9.26] — 2026-10-26

### Fixed

- 远程控制时被控端鼠标闪烁、操作发卡：Windows helper 对每条命令（包括每一次
  鼠标移动）都会调用 `OpenInputDesktop`，抓屏还用了 `CAPTUREBLT`（强制 DWM
  每次重绘，光标闪烁）。现在输入桌面最多每 250ms 刷新一次，抓屏改用普通
  `SRCCOPY`。
- helper 在普通桌面最多 15 fps（锁屏/Winlogon 仍为 8 fps），远程画面更跟手。
- 本版包含 0.9.25 的全部内容：AV1 全画质档位与每档 QP、AV1 状态徽标、每秒
  瓦片时间戳地图与可靠的丢帧补偿。

## [0.9.25] — 2026-10-26

### Added

- AV1 现在在所有画质档位启用，按档位设置 QP：无损/超清 QP 4（近无损）、平衡 22、
  流畅/游戏 32、极速 36（并先把矩形转成灰度）。小改动和文字仍走逐像素无损瓦片。
- 会话工具栏与底部状态栏显示绿色的 AV1 徽标；客户端 core 解码补丁后每秒上报一次
  （wire → decoded 字节数），5 秒没有补丁自动消失。

### Fixed

- 丢帧补偿可能永久丢失：`request-tiles`/`request-keyframe` 和 tile-refresh 响应
  现在走中继（TCP）可靠路径，不再受 UDP 丢包影响。
- 主机每秒向每个有画面的会话发送 128 px 单元的最新时间戳地图（tile clock）。客户端
  发现某个单元比已应用的新超过 1 秒，就重新请求该区域；这样即使一次刷新丢失，下一秒
  也会自愈，不会留下长期过期的区块。
- 主机端 `request-tiles` 增加 `MOLINK_AV1_LOG` 诊断输出。

## [0.9.24] — 2026-10-26

### Fixed

- Windows 的 0.9.23 安装包运行 AV1 会话时提示缺少 `fastfeat.dll`：vcpkg 的
  SVT-AV1 4.1 把 `third_party/fastfeat` 作为独立 DLL 链接，而打包只带了
  `SvtAv1Enc.dll` / `dav1d.dll`。Windows 包现在一并写入 `fastfeat.dll`；
  macOS 若存在 `libfastfeat` 也会一起放进 dmg。

## [0.9.23] — 2026-10-26

### Fixed

- 0.9.22 的 AV1 在 Windows/macOS 上编译失败：vcpkg 与 Homebrew 提供的是
  SVT-AV1 4.x，公开 API 与 Ubuntu 的 1.7 不同（句柄初始化参数、`EbSvtIOFormat`
  去掉宽高/色彩字段、`aq_mode`/`level_of_parallelism` 改名、预测结构枚举变了）。
  编码器现在按头文件实际形状选择调用：1.7 与 4.1 都能编译，并都跑通了编码/解码
  往返测试（4.1 需要 ALL_INTRA + hierarchical_levels≥2）。大屏编码线程数改为 4。
- libuvcpp 未启用 TLS 时（macOS CI 的核心 job）整个 TLS 配置块不再编译，
  改为报告“加密不可用”，不再因缺少 `ssl_error`/`ssl_context` 而构建失败。

## [0.9.22] — 2026-10-26

### Added

- AV1 自适应编码：流畅/游戏等有损档下，大面积变化（至少 2 个瓦片、矩形内
  至少 25% 是脏瓦片）会合并成矩形，用 SVT-AV1 编成独立 intra 关键帧，作为
  tile-refresh 补丁随基础帧发送；文字/小改动仍走原来的瓦片压缩。实测 1280×720
  全屏动画每帧补丁约 48 KB（原瓦片路径 2–7 MB/s）。
- 客户端 core 用 dav1d 在独立工作线程解码 AV1 补丁，再按 BGRA 瓦片交给页面，
  页面绘制逻辑不变；不支持的客户端不会协商 AV1。
- 能力协商：连接代理在 `hello` 里声明 `av1`（仅当本机 core 带 dav1d），主机
  只会在**所有**已认证会话都支持、且当前是有损档时启用 AV1。
- AV1 补丁和关键帧改走中继（TCP）可靠路径，避免 UDP 分片丢一个就整帧失效。
- 发布包开始包含 SVT-AV1（主机编码）与 dav1d（客户端解码）。**主机和客户端
  都要更新到 0.9.22**：旧客户端会继续走瓦片，旧主机不会发送 AV1。
- 无损/超清/平衡/原始档不使用 AV1，保持逐像素无损；AV1 只用于允许有损的档位。

## [0.9.21] — 2026-10-26

### Fixed

- 中继/ICE 会话在链路半开（NAT 或服务器静默断开、不发送关闭帧）时，画面和输入
  一起卡死且永远不会自动断开，只能手动重连。客户端现在检测两种“看起来还活着、
  实际已经没有响应”的情况并自动重连：
  - 连续 6 次 ping（约 18 秒）没有收到任何入站消息；
  - 连续 15 秒的拉帧请求都没有回应（主机仍回 pong，但画面/输入通路卡住）。
  重连沿用会话 token，不消耗验证码次数；旧 socket 的关闭事件不会干扰新连接。
- 纯客户端修复，主机保持 0.9.20 即可。

## [0.9.20] — 2026-10-26

### Fixed

- 通过设备 ID 的 ICE/P2P 会话“加载一帧后不再更新”。该路径的控制消息和帧都走
  UDP，实测有三处会让画面永久停住：
  - 客户端发出 `request-frame` 后只等回应、没有任何超时；请求或回应的单个数据包
    丢失后就不再拉帧。现在 1 秒未收到帧就重试。
  - 大尺寸关键帧经 UDP 分片重组可能超过 1 秒，被客户端的过期帧规则丢弃，而之后
    只有增量帧、没有底图。关键帧现在不受该规则限制。
  - 没有底图的会话现在每 3 秒补请求一次关键帧，直到画面出现。
- 握手同样走 UDP，丢失 `hello` 会让主机返回 `unauthorized`、丢失 `welcome` 会让
  会话一直停在“连接中”。客户端现在每 2 秒重发一次 `hello`（重复的 `hello` 在主机
  侧是幂等的，不消耗验证码次数），收到 `welcome` 后停止。
- 以上都是客户端修复，主机保持 0.9.19 即可；中继会话和局域网直连不受影响。

## [0.9.19] — 2026-10-26

### Fixed

- 主机与客户端的系统时钟不一致时（例如 Windows 主机比客户端慢 1 秒），客户端会把
  每一帧都判定为“在路上超过 1 秒”而丢弃，会话一直停在“等待第一帧”，但延迟和心跳
  正常。帧的新鲜度现在用主机时钟衡量：客户端取最近 10 秒内最快到达的帧估算主机与本机
  的时钟偏差，再据此判断帧年龄；丢弃过期帧并请求刷新的保护仍然有效。本地直连、ICE
  直连与中继会话都适用。
- 这是纯客户端修复，主机无需升级，更新客户端后重连即可出画面。

## [0.9.18] — 2026-10-26

### Fixed

- 0.9.17 的 Windows 发布包实际没有启用加密：CI 里 Chocolatey 把 OpenSSL 装到
  `C:\Program Files\OpenSSL`，而构建只查找 `C:\Program Files\OpenSSL-Win64`，
  `find_package(OpenSSL)` 失败后核心静默退化为“加密不可用”。现在 CI 会先探测已装
  路径，否则固定安装 OpenSSL 3.5.x 到 `C:\OpenSSL-Win64`，再作为
  `OPENSSL_ROOT_DIR` 传给核心；与 libuvcpp 归档里的 `libssl-3-x64.dll` /
  `libcrypto-3-x64.dll` 匹配。
- macOS 的 core 构建现在把 Homebrew `openssl@3` 前缀传给后端 CMake，dmg 里的
  设备证书/TLS 不再被静默禁用。
- 设置里的两个加密开关补充“修改后需要重启 moLink”的提示。

## [0.9.17] — 2026-10-26

### Added

- 每台设备首次启动生成自签 ECDSA P-256 证书与私钥（配置目录 `tls/`，私钥 0600），
  指纹在 ready/设置/欢迎消息里展示，供对端 pin；无需 CA。
- 两个独立的传输加密开关：
  - 远程会话（ID 服务器）默认开启；
  - 局域网会话默认关闭（因此默认不会出现自签证书提示）。
- 开启局域网加密后，28418 使用 HTTPS/WSS；Linux 原生壳层通过
  `load-failed-with-tls-errors`、Windows 壳层通过 WebView2
  `ServerCertificateErrorDetected`（ICoreWebView2_14）自动信任本机核心的证书；
  纯浏览器仍会显示证书警告，手动接受即可。
- 会话 tab 左侧和会话工具栏显示安全状态：绿色盾牌=端到端（直连证书），
  蓝色盾牌=中继 TLS，灰色盾牌=非加密；非加密会话有提示。
- TLS 模式下 ALPN 只声明 http/1.1，避免浏览器协商到本构建未启用的 h2。

### Changed

- OpenSSL 是可选依赖：有 OpenSSL 的构建提供设备证书/TLS；没有的构建保留设置但
  报告加密不可用，不会阻断发布。
- 中继内层端到端加密、28421 TLS 数据通道、28420 QUIC 与原生拖放桥仍待后续；
  LAN 加密默认关闭，现有连接不受影响。

## [0.9.16] — 2026-10-26

### Added

- 文件剪贴板自动同步：在本机（Linux/Windows 壳层或本机浏览器）复制文件后，core 读取
  本机剪贴板的文件列表（X11 `text/uri-list` / Win32 `CF_HDROP`），页面自动把文件传到
  远端，并把远端剪贴板设置为这些文件（Windows `CF_HDROP`，Linux `text/uri-list` /
  `x-special/gnome-copied-files`）。远端文件管理器或带剪贴板重定向的 RDP 客户端可以直接
  粘贴，不需要再打开文件面板点“复制到剪贴板”。仅本机 UI 可用（远端浏览器无法读取本机
  文件剪贴板）；单文件上限 64 MB、总计 256 MB，文件夹暂不自动同步。
- 回环接口 `/api/clipboard-files`（文件列表）和 `/api/clipboard-file?id=`（文件内容），
  id 是不可逆哈希；远端访问返回 403。X11 剪贴板新增文件列表读写（含 GNOME 格式回退）。
- 多会话并行：非当前 tab 保持会话连接并以 1 fps 慢速拉帧，切回时请求关键帧恢复全速；
  独立窗口不受影响。原来切走 tab 后画面停止拉取，看起来像远端被关闭。
- 文件拖入远端画面：拖放文件到会话画面会自动上传、放入远端剪贴板，并在可控制时发送
  Ctrl+V，把文件粘贴到远端当前聚焦的文件夹。拖动过程中的“占位文件映射”和从远端拖出
  到本机文件管理器需要主机侧虚拟文件系统/拖放代理（类似 RDP 驱动器重定向），当前的
  应用层协议无法凭空创建本机/远端文件系统对象；复制到本机仍走“远端复制 → 本机粘贴”
  的剪贴板通道（远端→本机文件剪贴板自动回传仍在后续版本）。
- 剪贴板焦点规则：远端 → 本机的文本只允许当前活动且获得焦点的会话写入，避免多个 tab
  互相覆盖本机剪贴板；本机 → 所有远端（文本和文件）不受焦点影响，所有 live 会话都会
  推送，后台 tab 也一样。

### Fixed

- 语音通话麦克风：
  - Windows 主机的 WASAPI 采集/播放以前只接受 48 kHz 的混音格式，44.1 kHz 的耳机或
    非 48 kHz 麦克风会被判定为“不可用”。现在用 `AUTOCONVERTPCM` 请求固定的
    48 kHz 单声道 16-bit，由音频引擎自动重采样。
  - 客户端会区分“页面不是安全上下文（HTTP 远程访问）”“权限被拒”“没有设备”
    “被占用”等原因并显示对应提示，不再只有一个笼统的“未获得麦克风权限”。
  - Linux 原生壳层额外打开 WebRTC；Windows 壳层的麦克风权限放宽到实际加载的
    核心 origin（不再只认 127.0.0.1/localhost）。
- 挂断/被拒时主机麦克风不停止：`call_hub` 在通知前清空了 `session_id`，导致会话
  侧的事件处理被会话号过滤掉。`ended`/`declined` 现在广播给所有会话，主机会真正
  停止采集和播放。语音的启动/停止也统一转到 app loop 线程执行（接受/拒绝来自
  HTTP 工作线程），避免 WASAPI COM 对象和 WebSocket 跨线程访问。
- 通话悬浮栏增加了拖动手柄，可在画面内任意移动（自动限制在可视区域内），不再固定
  挡住左上角。
- 多人同时远程时的聊天身份：`hello` 现在可携带客户端的计算机名和设备 ID，聊天消息
  带 `sender_name` / `sender_id` / `sender_session`；会话聊天和主机聊天窗口都会显示
  每条消息的发送者名称和 ID，自己发的消息按会话号右对齐。纯浏览器访问没有设备 ID
  时按来源 IP 标注。

### Changed

- 文件数据在 P2P 升级后继续走中继的 WebSocket/TCP：帧、语音和输入留在 ICE/UDP（丢包可
  通过关键帧/瓦片刷新恢复），`file_data` 包固定走可靠 TCP，避免 UDP 分片丢失导致文件
  传输卡死。中继在切到 P2P 后保持，中继断开时文件回退到 UDP，会话本身不受影响。
- Windows 7 客户端强制 TCP：按设备 ID 连接时走中继，直连地址时走直连 WebSocket，跳过
  ICE/UDP。该选择在手动重连后保持。
- 简易版设备列表（最近/已保存/局域网/联网）增加翻页：卡片每页 6 个、列表每页 10 条，
  底部显示页码和上一页/下一页，切换标签或视图时回到第 1 页。

## [0.9.15] — 2026-10-26

### Fixed

- Windows 安装后台服务后，采集实际由 `molink-helper.exe` 完成；它没有做 DPI 感知，
  150% 缩放的 2560×1440 屏幕仍会按 1707×960 的逻辑像素采集和注入（0.9.13 只修了
  `molink-core` 本身）。helper 现在同样优先 per-monitor v2（Win8.1 回退 per-monitor，
  Win7 回退系统 DPI），按物理像素抓屏和映射输入。
- 多次验证码被重连提前耗尽：每次 `hello` 都扣一次使用次数，网络抖动或手动重连几次后
  多次码就会换成新码，只能反复去主机取码。`welcome` 现在下发 `session_token`，重连的
  `hello` 带上它即可免扣次数重新鉴权；对已鉴权连接的重复 `hello` 也不会再次扣减。
- 分辨率菜单只在采集后端真的支持切换时显示；`display-mode` 被拒绝时给出明确提示，
  不再显示无意义的“重新连接”。连接质量水印现在同时显示采集后端
  （`dxgi-duplication` / `secure-desktop` / `gdi` 等），便于判断主机采集路径。

### Changed

- 简易版连接框会记住上次输入的设备 ID/地址和验证码（写入 core 的 `ui.json`，跨启动
  保留），最近/已保存设备也会带上各自的验证码；设备卡片改为单击把 ID（和记住的验证码）
  填入连接框、双击才直接连接；验证码以掩码显示，不能查看。

## [0.9.14] — 2026-10-26

### Added

- **中继优先**：按设备 ID 连接先通过目录的 `WS /v1/relay` 建立会话（通常 1–2 秒），
  同时后台继续 ICE 打洞；成功后自动切换到 P2P，失败则保持中继。
- **强制中继**：连接页勾选后完全跳过 ICE，会话固定走中继；勾选状态持久化。
- 会话页底部显示当前通道（中继 / 直连），切换时自动更新。

## [0.9.13] — 2026-10-26

### Fixed

- Windows 高 DPI / 每显示器缩放（例如 112.5%）下，捕获和输入使用虚拟化后的逻辑尺寸，
  远端分辨率会显示成 1707×960 之类的错误值，点击坐标也会偏移。现在优先使用
  per-monitor v2 DPI awareness（Win7 回退到系统 DPI awareness），捕获与输入都按物理像素。

## [0.9.12] — 2026-10-26

### Added

- 偏好设置 → 安全 新增「STUN 服务器」：`host[:port]`，留空使用默认
  `stun.miwifi.com:3478`，填 `none` 关闭；保存后下一次按 ID 连接生效。

## [0.9.11] — 2026-10-26

### Fixed

- 客户端默认启用 STUN（`stun.miwifi.com:3478`）。之前未设置 `MOLINK_STUN_HOST` 时只收集
  内网候选，异地（跨公网）按 ID 连接根本打不通，对方也不会收到访问请求；
  `MOLINK_STUN_HOST=none` 可关闭，其它值可覆盖。

## [0.9.10] — 2026-10-25

### Changed

- 同一局域网内按 ID 连接时，优先直接使用目录返回的私网地址建立会话（实测 welcomed 约
  0.02 秒、首帧约 0.6 秒）；直连失败才回退到核心的 ICE 代理。跨公网仍走 ICE/中继。

## [0.9.9] — 2026-10-25

### Fixed

- 主机空闲锁定后，携带**正确验证码**的远程会话不再被直接拒绝（以前在验证码校验之前就返回
  “locked”，导致远程端一直连不上）；现在先校验验证码，通过后自动解锁并建立会话。
  不带验证码的访问请求在锁定期间仍然被拒绝。

## [0.9.8] — 2026-10-25

### Changed

- 按设备 ID 连接的会话窗口只显示**设备 ID 与计算机名称**，不再显示对方的内网 IP 和端口
  （标签页提示和最近记录同样显示 ID）；只有 IP/局域网直连才显示地址。

### Fixed

- 全屏时悬浮工具栏在浅色主题下文字与黑色背景同色、看不清：工具栏改为固定的白字深底配色，
  按钮、输入框和标签统一可读。

## [0.9.7] — 2026-10-25

### Fixed

- Windows 7 没有 WebView2 时，`moLink.exe` 不再直接报
  “could not create the WebView2 environment” 退出：内核照常启动，程序自动在默认浏览器
  打开带 client token 的 UI，并提示安装 WebView2 Runtime 109 或使用 Firefox ESR 115 /
  Chrome 109；关闭 moLink 窗口会停止后台服务。

### Added

- 支持把**固定版 WebView2 运行时**放在程序目录的 `WebView2Runtime\`（Win7 离线包使用）：
  存在时优先用它，不需要系统安装 Evergreen 运行时。

## [0.9.6] — 2026-10-25

### Added

- 最近连接记录支持逐条删除（卡片/列表上的垃圾桶按钮），以及「清空最近记录」（带确认）；
  已保存设备仍通过星标取消收藏。

## [0.9.5] — 2026-10-25

### Fixed

- 授权码错误时不再把它当成 ICE 失败去回退直连并反复重试：会话页直接显示「授权码不正确，
  请重新输入」，并提供输入框重新连接；`unauthorized`/`locked`/`access_denied` 等策略
  性关闭不再触发直连回退。

### Changed

- 目标端待处理信令轮询从 2 s 降到 500 ms（目录允许 8 req/s/设备），入站呼叫几乎立即可见，
  按 ID 连接明显更快。

## [0.9.4] — 2026-10-25

### Fixed

- 设备 ID 查询改由本机核心代理（`/api/directory/device`、`/api/directory/devices`）：
  浏览器直接 `fetch` 目录时，需要浏览器自己信任目录的私有（Cloudflare Origin）证书；
  没安装该 CA 的机器会报「无法连接 ID 服务器」，即使 ICE 会话已经建立。现在查询由核心
  带着 CA 完成，浏览器只访问本机核心。

### Changed

- 包含 0.9.3 的改动：简洁模式本机 ID 字号 20px；默认单次验证码（1 次）

## [0.9.3] — 2026-10-25

### Changed

- 简洁模式首页的本机设备 ID 字号从 13px 提高到 20px（加粗、加宽字距），瞄一眼就能读出来
- 默认临时验证码改为**单次**（`temp_mode=single`，1 次）；设置里自定义验证码的默认次数
  也从 20 改为 1。已有的 `settings` 文件保持原值，可在「偏好设置 → 安全」里改

## [0.9.2] — 2026-10-25

### Fixed

- Windows 安装包现在附带与构建工具集匹配的 VC 运行时。libuvcpp 1.8.5 包内自带的
  `MSVCP140.dll` 是 14.29，而内核用新工具集构建，加载它会在启动时崩溃（事件日志
  指向 MSVCP140 的 0xc0000005），外壳因此报
  “molink-core did not report a ready port and tokens”。打包时改用机器上最新的
  Redist CRT 覆盖。

### Changed

- Win7 x86 构建改用 libuvcpp **1.8.5 kit**（`uvcpp-kit-1.8.5-mingw32.zip`）：它的
  `prebuilt/` 就是 full 档（web + webapp），`build-win7-x86.ps1` 默认拿它当
  `MOLINK_UVCPP_PREFIX`，不再从源码编 libuvcpp；cmake 与 zlib 静态库仍取自 kit 的
  `deps/`。
- Win7 x86 不再单独发版：`release-desktop.yml` 新增 `win7-x86` job，与 Linux/Windows/
  macOS 一起附加到同一个 `v*` Release；旧的 `release-win7.yml` 与 `win7-x86-v*`
  标签停用。

## [0.9.1] — 2026-10-25

### Changed

- 内置目录 `https://molink.ssvr.top:28430` 使用 Cloudflare Origin 证书，客户端现在随包
  携带该 CA：`server_ca` 留空且使用内置目录时自动校验（根证书写入配置目录，
  设置里仍显示为空，不会写入 `server_ca`）；自定义目录留空仍使用系统信任库

## [0.9.0] — 2026-10-25

### Changed

- 空闲锁定只作用于非本机访问：本机壳层与浏览器保持可用，远程浏览器在无操作后
  才重新要求验证码；WebSocket 会话同样按来源区分
- 简洁模式不再显示域组与远程目录开关，两者移到高级模式的「偏好设置 → 服务端」；
  简洁模式同时隐藏目录 URL，并压缩了整体页面高度
- 限制浏览器的拖放行为：只有文件发送面板接受拖入文件，其他位置的文本 / 文件 /
  HTML 拖放会被拦截，避免 WebView 被拖入的文档替换
- 设备目录上报本机访问目录所用的局域网地址；目录只在请求方与目标共享同一公网出口时
  返回该地址，避免把内网地址暴露给外网

### Fixed

- 新会话首帧：捕获的第一帧序号为 0 时，会话重放把 `-1` 当成“没有内容”，新连接会一直收到
  0 瓦片空帧；现在从找到的关键帧（包括第 0 帧）开始重放

### Added

- 按设备 ID 连接先经本机核心的 `/ws/connect` 尝试 ICE：libjuice 使用
  external-socket 模式（`uvcpp_udp` + `uvcpp_timer`，不额外建线程），成功后会话的
  text/MOLK 帧走 ICE 选中的 UDP 通道；ICE 失败自动回退直连 WebSocket
- UDP 会话层按 1200 字节分片/重组，大关键帧不再受单数据报 MTU 限制
- `MOLINK_STUN_HOST` / `MOLINK_STUN_PORT` 环境变量用于配置 STUN（不设置时仅主机候选）
- `MOLINK_P2P_PORT` 固定 ICE 套接字的本地 UDP 端口（默认临时端口），方便防火墙放行

### Build

- CI 与发布工作流直接下载 libuvcpp 预编译包（`UVCPP_VERSION`，默认 1.8.5），
  不再为 Linux / Windows 从源码构建，缩短构建时间；macOS 仍从源码构建
- Win7 x86 改用 1.8.5 的 `uvcpp-win7-x86-mingw32.zip`（已含 WebApp），不再从
  1.8.3 源码重建 libuvcpp；zlib 仍取 1.8.3 kit 的静态库（molink 自己调用 zlib）

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
