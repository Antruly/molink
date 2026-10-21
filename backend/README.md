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
| `--print-ready` | 就绪后向 stdout 输出一行 JSON |

## 结构

```
src/
├── main.cpp              启动、信号、父进程监视
├── config.*              命令行解析、令牌生成、恒定时间比较
├── api.*                 CORS、Bearer 鉴权、/healthz 与 /api/*
├── session_registry.*    会话记录（shared_ptr，跨回调安全）
├── ws_session.*          /ws/session 处理器、心跳与回收线程
├── test_pattern.*        合成测试帧（真实抓屏接入前的临时源）
├── protocol/frame_packet.*  MOLK 编解码（无 libuvcpp 依赖）
└── util.*                会话号、时间
tests/frame_packet_test.cpp   协议测试（36 项断言）
```

## 已知问题

- 安装的 `libuvcpp.so` 依赖同目录 `libllhttp.so` 但没有 `RUNPATH`。本工程用
  旧式 `RPATH`（`--disable-new-dtags`）让构建树可运行；发布包必须把共享库
  一并打包。
- 真实屏幕捕获与输入注入尚未实现；`/api/info` 的 `capabilities` 会如实报告。
