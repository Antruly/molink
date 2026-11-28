# 排障与崩溃取证

这份文档只讲一件事：**程序出问题时，怎么把现场留下来**。没有现场就只能猜，过去两天里有两次崩溃就是因为 dump 读不到而卡住。

## 崩溃转储在哪

程序自己写，不依赖系统的 `coredumpctl` / WER：

| 平台 | 目录 |
|---|---|
| Windows | `%APPDATA%\moLink\crashdumps\`（服务账户下是 `C:\Windows\System32\config\systemprofile\AppData\Roaming\moLink\crashdumps\`） |
| Linux / macOS | `~/.config/moLink/crashdumps/`（服务模式在服务账户的家目录下） |

每次崩溃产生两类文件：

```
crash-<pid>-<unix秒>.txt      # 两边都有
core-<pid>-<unix秒>.dmp       # 只有 Windows 有
```

`.txt` 是纯文本，头部固定几行：

```
molink crash dump
signal=11 SIGSEGV
addr=0x3e8000ff3f7
pid=1045017
started=2026-11-28T21:03:38+0800
version=0.12.33
frames=8
frame0=0x5c180d7a962e
frame1=0x728dfa045330
...
```

## 怎么读

### Linux：把地址翻译成函数

```bash
# 1. 找到对应版本的 core（必须是同一个构建，否则地址对不上）
# 2. 一次性翻译全部帧
addr2line -f -C -e ./molink-core $(grep '^frame' crash-1045017-1795871038.txt | cut -d= -f2)
```

有调试符号的构建（`-DCMAKE_BUILD_TYPE=RelWithDebInfo`）能直接出文件名和行号；Release 构建只能给出函数名，至少能定位到模块。

### Windows：用 cdb / WinDbg 打开 minidump

```
cdb -z core-50948-1795871038.dmp -c ".ecxr; kb" 
```

（`cdb` 随 Windows SDK 的 Debugging Tools 提供。）`.ecxr` 切到异常上下文，`kb` 打栈；要完整栈用 `kv`。

## 提交问题时带什么

1. 上表目录里的 `crash-*.txt` / `core-*.dmp`
2. 同时刻的日志（`%APPDATA%\moLink\logs\molink-*.log` 或 `~/.config/moLink/logs/`）
3. 崩溃前在做什么：有没有远程会话、是否长时间空闲、ID 服务器是否连得上

## 已知会崩溃的路径（截至 0.12.33）

- **WS 发帧**（已修）：会话断开后仍向已释放连接发视频帧，崩在 zlib。0.12.32 起用 libuvcpp 1.12.0 的 `retired` 守卫 + 会话侧先停推送。
- **ID 服务器心跳**（未结案）：服务器连不上时，心跳线程里 `uvcpp_http_client::connect_wait` 的 `uv_run` 偶发 SIGSEGV（issue #163）。0.12.33 起会留下 `crash-*.txt`，下次出现请按上面三样一起提。

## 服务模式没有桌面会话时

终端会明确报 `terminal_unavailable`，不会静默用服务账户起 shell —— 这是设计行为，不是故障（见 `docs/protocol.md` 的终端一节）。
