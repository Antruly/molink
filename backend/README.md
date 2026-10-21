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
| `--host` | 绑定地址，默认 `127.0.0.1` |
| `--port` | 端口，默认 `21118` |
| `--token` | 访问令牌；缺省随机生成（也可用 `MOLINK_TOKEN`） |
| `--allow-remote` | 允许绑定非回环地址 |
| `--parent-pid` | 监视父进程，父进程消失时退出 |
| `--capture-fps <n>` | 抓屏帧率上限，默认 15 |
| `--no-capture` | 强制使用合成测试帧 |
| `--no-input` | 禁用指针与键盘注入 |
| `--no-compress` | 关闭 zlib 瓦片压缩，发送原始 BGRA |
| `--no-clipboard` | 关闭剪贴板同步 |
| `--print-ready` | 就绪后向 stdout 输出一行 JSON |

`MOLINK_DISPLAY` 可指定 X display（默认沿用 `DISPLAY`）。

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
- 结果通过 `ws_options::post` 投回事件循环再发送，剪贴板线程不碰连接状态。
- `--no-clipboard` 可关闭。

## 结构

```
src/
├── main.cpp              启动、信号、父进程监视
├── config.*              命令行解析、令牌生成、恒定时间比较
├── api.*                 CORS、Bearer 鉴权、/healthz 与 /api/*
├── session_registry.*    会话记录（shared_ptr，跨回调安全）
├── ws_session.*          /ws/session 处理器、心跳、帧回放与回收线程
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
