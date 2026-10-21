# molink-core

C++20 + libuvcpp 后台。HTTP/WS 接口、会话管理、MOLK 瓦片帧编解码。协议见
[../docs/protocol.md](../docs/protocol.md)。

## 构建

需要 libuvcpp ≥ 1.7，且编译时开启 `UVCPP_BUILD_WEB=ON` 与
`UVCPP_BUILD_WEBAPP=ON`（WebSocket 依赖两者）。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

libuvcpp 不在标准前缀时：

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/uvcpp
```

只跑协议测试（不需要 libuvcpp）：

```bash
cmake -S . -B build -DMOLINK_BUILD_CORE=OFF
cmake --build build && ctest --test-dir build --output-on-failure
```

## 运行

```bash
./build/molink-core --print-ready
```

| 参数 | 说明 |
|---|---|
| `--host` | 绑定地址，默认 `0.0.0.0`；默认仍拒绝非回环连接（安全设置可热切换）；显式 `127.0.0.1` 则真正只绑定本机 |
| `--port` | 网页/API/WS 端口，默认 `28418`；`0` 表示由系统分配 |
| `--http3-port` | HTTP/3（QUIC）网页/API 端口，默认 `28419`（预留） |
| `--quic-port` | QUIC 直连数据端口，默认 `28420`（预留） |
| `--tcp-data-port` | TCP 直连数据端口，默认 `28421`（预留） |
| `--udp-port` | UDP 直连数据端口，默认 `28422`（预留） |
| `--token` | 访问令牌；缺省生成并持久化到配置目录（也可用 `MOLINK_TOKEN`） |
| `--allow-remote` | 确认显式指定的非回环 `--host`（默认地址无需此参数） |
| `--parent-pid` | 监视父进程，父进程消失时退出 |
| `--capture-fps <n>` | 抓屏帧率上限，默认 15（也可由客户端 `capture-fps` 自适应调整） |
| `--tile-size <n>` | 瓦片边长，默认 0 = 自动（8+ 线程 128、4–7 线程 256、否则 512） |
| `--change-threshold <n>` | 每通道差异阈值，默认 4；0 = 逐字节精确比较 |
| `--no-capture` | 强制使用合成测试帧 |
| `--no-input` | 禁用指针与键盘注入 |
| `--no-compress` | 关闭 zlib 瓦片压缩，发送原始 BGRA |
| `--no-clipboard` | 关闭剪贴板同步 |
| `--web-root <dir>` | 在 `/` 托管构建好的前端（默认自动查找 `frontend/dist`） |
| `--lock-timeout <s>` | 空闲锁定秒数，默认 60；0 = 从不（也可由界面修改并持久化） |
| `--print-ready` | 就绪后向 stdout 输出一行 JSON |

`MOLINK_DISPLAY` 可指定 X display（默认沿用 `DISPLAY`）。

未显式传入端口参数时，核心会读取配置目录里的 `tcp_port`、`http3_port`、
`quic_port`、`tcp_data_port`、`udp_port`（由界面「偏好设置 → 网络端口」或
配置文件写入），再退回默认值。命令行参数优先。

端口段避开 RustDesk 的 21115–21119：`28418` 网页/API/WS（HTTP/1.1 + WebSocket）、
`28419` HTTP/3、`28420` QUIC 数据、`28421` TCP 数据、`28422` UDP 数据。
`GET /api/transports` 返回当前 `available` 的传输和完整端口表；`welcome` 里也有
`transports` / `transport_ports`。浏览器永远从 WebSocket 起步；HTTP/2（h2c）与
QUIC/HTTP3 数据面在验证通过前保持 `available=false`。

远程验证失败按 IP 计数并阶梯封禁：每轮 5 次失败，锁定时长依次为 30 秒、
2 分钟、10 分钟、1 小时、6 小时，第 6 轮永久封禁并写入 `banned_ips`。
24 小时无新失败会重置阶梯；回环地址不计数。HTTP `/api/login`、其他 `/api/*`
的错误 Bearer 令牌与 WebSocket `hello` 共用同一份额度。

## Web UI 托管

`--web-root`（或 `MOLINK_WEB_ROOT`）指向构建产物时，核心用 libuvcpp 的静态文件
服务把前端挂在 `/`：ETag/304、Range、路径穿越防护都由框架处理。查找顺序是
显式参数、环境变量、`frontend/dist`、可执行文件旁的 `web/` 与 `../share/molink/web`。

桌面壳就是靠这条路径把 UI 加载进 WebKitGTK；不装壳时浏览器直接访问
`http://127.0.0.1:28418/?token=...` 即可。

## 认证、设置与发现

- 配置文件：`<配置目录>/settings`，权限 0600，包含：
  - `allow_remote_access`（默认 0）：允许非回环连接；关闭时非回环 TCP 在 HTTP 解析前立即关闭，打开/关闭都无需重启；
  - `allow_web_client`（默认 0）：允许非回环浏览器加载 Web UI / 登录页；
  - `discovery_enabled`（默认 0）：UDP 组播发现，必须先开 `allow_remote_access`；
  - `device_name`；
  - `long_term_code`：至少 8 位，必须同时含字母和数字；
  - `temp_mode` / `temp_code` / `temp_uses` / `temp_remaining`：主页临时码，single 或 multi。
- 临时码：single 用一次后自动换新；multi 每次成功登录扣一次，扣到 0 自动换新。自定义码 4–64 位、不能含空白/控制字符。
- 登录：`POST /api/login` 接受长期码或临时码，返回随机 `session_token`；之后的 `/api/*` 只接受 session token，避免授权码被反复当作 Bearer。WebSocket `hello` / `?token=` 接受 session token 或授权码。
- 壳层身份：`--print-ready` 额外输出 `client_token`；`allow_web_client=0` 时，回环 `GET /`、`/api/state`、`/api/login`、`/api/device`、`/api/transports` 需要 `X-MoLink-Client: <client_token>` 或初始 URL 的 `client=`。普通本机浏览器拿不到这个 token，因此默认无法打开 UI。
- `/api/device`（仅回环）返回本机会话 token、临时码状态和设备名；`/api/settings`、`/api/auth/code`、`/api/auth/long-term` 仅回环可用；`/api/discovery` 也仅回环。
- 发现：UDP `28423`，组播组 `239.255.42.99`，beacon 每 2 秒一次，6 秒过期，只含 `device_id/name/port/protocol`，不含 token。
- 迁移：旧版本持久化的 `token` 文件会作为首次启动的长期授权码写入 `settings`；之后以 `settings` 为准。

## 传输抽象

- `src/transport/session_transport.h`：文本/二进制发送、关闭、对端地址、入站回调的纯接口。
- `src/transport/ws_transport.*`：WebSocket 实现，只借用 `uvcpp_ws_connection`，连接关闭时清空回调。
- `src/session_controller.*`：hello、`request-frame`、`capture-fps`、输入、剪贴板、光标、帧回放和关闭回收，全部只调用 `session_transport`。
- `src/session_registry.*`/`session_supervisor`：用接口发帧、发心跳和关闭，不再认识 WebSocket 类型。

接 TCP/QUIC 时只需要：
1. 实现一个 `session_transport`（负责自己的分帧/消息边界）；
2. 在监听器里创建它和 `session_controller`，调用 `start(initial_token)`；
3. 把对端从 socket 读到的文本/二进制喂给控制器。

鉴权、限速、锁定、瓦片选择、压缩、输入和剪贴板逻辑无需复制。

## 多显示器

`capture_source` 暴露 `displays()` / `current_display()` / `current_display_info()` / `select_display(index)`。切换后采集线程重建图像并发布关键帧；`input_injector::set_display_rect()` 按所选显示器重算输入坐标。

- X11：动态加载 XRandR 的 `XRRGetMonitors`，按 monitor 矩形抓取并裁剪 Damage 事件；
- Windows：枚举 `IDXGIAdapter::EnumOutputs`，为每个输出重建 Duplication，SendInput 使用 `MOUSEEVENTF_VIRTUALDESK`；
- macOS：`CGGetActiveDisplayList` 枚举，`CGDisplayCreateImage(displayID)` 抓取，CGEvent 使用该显示器的 `CGDisplayBounds`。

默认 index 0 是主显示器；显示器列表是主机级共享的，一个会话切换会影响其他会话。

## 平台后端

| 平台 | 抓屏 | 输入 | 剪贴板 |
|---|---|---|---|
| Linux | X11 + XShm + XDamage（`capture/x11_capture.cpp`） | XTEST（`input/x11_input.cpp`） | X11 selection（`clipboard/x11_clipboard.cpp`） |
| Windows | DXGI Desktop Duplication（`capture/windows_capture.cpp`） | SendInput（`input/windows_input.cpp`） | `CF_UNICODETEXT`（`clipboard/windows_clipboard.cpp`） |
| macOS | CoreGraphics（`capture/macos_capture.mm`） | CGEvent（`input/macos_input.cpp`） | NSPasteboard（`clipboard/macos_clipboard.mm`） |

抓屏不可用时统一回退到合成测试帧。macOS 首次抓屏需要“屏幕录制”权限，
输入注入需要“辅助功能”权限；未授权时 `capabilities` 会如实为 false 或画面为测试帧。

## 访问与锁定

- 回环地址访问免验证；`/api/device` 只向回环返回设备 ID 与验证码。
- 非回环访问必须 `POST /api/login` 验证验证码；通过后才能访问 `/api/*` 与 WebSocket。
- 空闲锁定在 `session_supervisor` 中判定：没有已鉴权 WS、所有 UI 窗口上报的
  会话数为 0、且无操作超过 `lock_timeout` 时锁定。锁定时 `/api/*` 返回 423，
  WS 握手以 `1008 locked` 拒绝。
- 登录失败 5 次后暂停 30 秒；锁定时间通过 `/api/lock-timeout` 修改并写入配置目录。

## 屏幕捕获

```
src/capture/
├── capture.h              capture_source 接口 + 有界帧历史
├── capture.cpp            历史/replay、启动时选择后端
├── x11_capture.cpp        Linux X11 / XShm / XDamage
└── synthetic_capture.cpp  合成测试帧（回退）
```

- **X11 + XShm**：整屏抓取走 `XShmGetImage`，共享内存避免一次拷贝。
- **XDamage**：优先按受损矩形抓取；Cinnamon 等合成器通常把整屏标记为受损，
  此时退化为整屏抓取，但仍只发送变化瓦片。
- **兜底节奏**：即使没有 damage 事件，也每 500 ms 抓一次，保证画面不会冻结。
- **无会话暂停**：没有已鉴权会话时抓屏线程休眠，CPU 接近 0%。
- **有界历史**：保留最近 120 帧 / 128 MiB，客户端落后时按序补发，避免反复
  请求 22 MB 关键帧。
- **周期全量刷新**：每 30 秒一次，修正可能漏掉的 damage。
- **瓦片压缩**：zlib 级别 1；压缩后不比原始小就保持 `bgra`，帧头 `codec`
  如实标记 `deflate_bgra`(5)。
- 仅处理 32 位 BGRA 最快路径；其它像素格式走通用掩码转换。

性能参考（本机 2880×1920、X11、depth 24、桌面持续有变化）：

| 指标 | 原始 BGRA | zlib 瓦片 |
|---|---:|---:|
| 带宽 | 94 Mbps | **7.4 Mbps** |
| 单核 CPU（活跃会话） | ~35% | ~24% |
| 无会话空闲 | 0% | 0% |

## 光标

`XGetImage` 不包含光标。X11 后端通过 XFixes（同样 `dlopen`）取得光标位图与位置，
随帧请求以 `cursor` / `cursor-position` 控制消息发给客户端，由前端绘制覆盖层。
形状只在 `serial` 变化时重发。

## 输入注入

```
src/input/
├── input.h         input_injector 接口
└── x11_input.cpp   XTEST（dlopen libXtst.so.6）
```

- 坐标由客户端归一化到 `[0,1]`，服务端按当前屏幕尺寸换算。
- `KeyboardEvent.code` 映射到 X keysym（字母、数字、方向键、修饰键、标点、F1–F24）。
- 组合键由客户端分别发送按下/抬起，X 服务器据此产生大写与符号。
- 会话断开（且没有其它已鉴权会话）时释放所有按键与鼠标按钮，避免远端卡住修饰键。
- 没有 X display 或服务端不支持 XTEST 时，`capabilities.input_injection` 为 false，
  客户端隐藏控制按钮。
- `--no-input` 可完全关闭注入。

## 剪贴板

```
src/clipboard/
├── clipboard.h
└── x11_clipboard.cpp   独立线程 + 独立 Display 连接
```

- `set`：取得 CLIPBOARD 所有权，响应 SelectionRequest（TARGETS/UTF8_STRING/STRING）。
- `get`：XConvertSelection 异步读取，支持 INCR 分段；超时 2 秒。
- 文本上限 192 KiB（受 X11 单次属性写入大小限制）。
- 结果通过 `session_options::post` 投回事件循环再发送，剪贴板线程不碰连接状态。
- `--no-clipboard` 可关闭。

## 结构

```
src/
├── main.cpp              启动、信号、父进程监视
├── config.*              命令行解析、令牌生成、恒定时间比较
├── api.*                 CORS、Bearer 鉴权、/healthz 与 /api/*
├── session_registry.*    会话记录（shared_ptr，跨回调安全）
├── session_controller.*  传输无关的会话状态机（hello/帧/输入/剪贴板）、心跳与回收
├── transport/            session_transport 接口 + WebSocket 实现
├── identity.*            设备 ID、令牌与锁定设置的持久化
├── lock_state.*          本机免验证、远程登录与空闲锁定状态
├── capture/              屏幕捕获（X11/XShm/XDamage、合成回退、帧编码）
├── input/                指针/键盘注入（XTEST）
├── clipboard/            文本剪贴板（X11 selection）
├── compression.*         zlib 瓦片压缩
├── test_pattern.*        合成测试帧
├── protocol/frame_packet.*  MOLK 编解码（无 libuvcpp 依赖）
└── util.*                会话号、时间
tests/frame_packet_test.cpp   协议测试（36 项断言）
```

## 已知问题

- 安装的 `libuvcpp.so` 依赖同目录 `libllhttp.so` 但没有 `RUNPATH`。本工程用
  旧式 `RPATH`（`--disable-new-dtags`）让构建树可运行；发布包必须把共享库
  一并打包。
- libuvcpp 的监听 socket 未设置 `SO_REUSEADDR`，进程退出后立即重启可能得到
  `EADDRINUSE`，需要等待 TIME_WAIT 结束或换端口。
- 输入注入尚未实现；`/api/info` 的 `capabilities` 会如实报告。
- 编码器尚未接入，帧数据为原始 BGRA，带宽与 CPU 都偏高。
