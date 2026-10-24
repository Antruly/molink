# 上游反馈草稿（uvp / libuvcpp）

本文件记录本次开发中实际遇到的上游问题，附复现命令与建议修复，便于提交 issue 或 PR。
没有在本机之外提交任何内容。

环境：Linux x64，gcc 13.3，CMake 3.28，Ninja 1.11，libuvcpp 1.8.0（安装前缀
`~/.local/uvcpp-uvscope`），uvp MCP。

---

## 1. uvp_build：构建成功但返回结构化输出校验错误

**现象**

`uvp_build` 连续两次返回：

```
Structured content does not match the tool's output schema: data must NOT have additional properties
```

同时 `backend/build/` 下出现了 `build.ninja`、`.ninja_log`、`molink-core`、
`molink-core-protocol-test`，说明构建**实际执行了**，只是返回体不符合声明 schema。

**复现**

1. `uvp_workspace_create` 生成一个工作区（`websocket-echo` 模板），随后替换
   `CMakeLists.txt`，增加第二个可执行目标与 `enable_testing()`。
2. 调用 `uvp_build`，参数仅 `path` 与 `build_type`（第一次还带了 `generator`、
   `jobs`，同样报错）。
3. 观察：调用返回 schema 错误；检查 `build/`，产物已生成。

**建议**

在返回前用工具声明的 output schema 校验结果，或收紧返回对象（剔除未声明的字段）。
最小修复是让这类校验失败降级为 `limitations`/警告，而不是丢弃整个结果——目前调用方
拿不到任何构建诊断。

---

## 2. libuvcpp 安装产物缺少 RUNPATH，导致间接依赖找不到

**现象**

```
$ readelf -d ~/.local/uvcpp-uvscope/lib/libuvcpp.so | grep -E 'NEEDED|RUNPATH'
 (NEEDED) 共享库：[libuv.so.1]
 (NEEDED) 共享库：[libllhttp.so.9.2]
 ... （无 RUNPATH / RPATH）

$ readelf -d backend/build/molink-core | grep RUNPATH
 (RUNPATH) Library runpath: [/home/zwy/.local/uvcpp-uvscope/lib]

$ ./backend/build/molink-core
error while loading shared libraries: libllhttp.so.9.2: cannot open shared object file
```

`libllhttp.so.9.2` 与 `libuvcpp.so` 同在 `~/.local/uvcpp-uvscope/lib`。可执行文件的
`DT_RUNPATH` **不会**传递给间接依赖，而 `libuvcpp.so` 自身没有任何搜索路径，于是
加载失败。

**建议修复（libuvcpp 的 CMake）**

给安装的共享库写入 `$ORIGIN`：

```cmake
if(UNIX AND NOT APPLE)
  set_target_properties(uvcpp PROPERTIES
    INSTALL_RPATH "$ORIGIN"
    BUILD_WITH_INSTALL_RPATH TRUE)
elseif(APPLE)
  set_target_properties(uvcpp PROPERTIES
    INSTALL_RPATH "@loader_path"
    BUILD_WITH_INSTALL_RPATH TRUE)
endif()
```

并对安装的其它共享依赖（llhttp 等）做同样处理。这样安装前缀可整体移动，无需
`LD_LIBRARY_PATH`。

**本项目的临时规避**

`backend/CMakeLists.txt` 对可执行文件使用旧式 `RPATH`（`-Wl,--disable-new-dtags`）
并加入 `$ORIGIN`：

```cmake
target_link_options(molink-core PRIVATE
  "LINKER:--disable-new-dtags" "LINKER:-rpath,$ORIGIN")
```

这是项目侧的绕行，不是对上游缺陷的修复。

---

## 3. uvp_verify 无法给被测进程传环境变量

**现象**

`uvp_run` 有 `env` 参数，`uvp_verify` 只有 `run.args`。本项目首次启动随机生成 32 位
十六进制令牌（也支持 `MOLINK_TOKEN`），`uvp_verify` 因此无法注入已知令牌，也就无法
断言 `/api/*` 的鉴权路径；这些断言只能在 `uvp_verify` 之外用 `uvp_http_request` 手工验证。

**建议**

给 `uvp_verify` 的 `run` 增加 `env: {string: string}`，与 `uvp_run` 对齐。

---

## 4. uvp_workspace_create：`health` 模块与 `websocket-echo` 模板冲突

**现象**

```
invalid_request: The requested modules conflict with the template: the health module
registers GET /healthz, which the websocket-echo template already serves
```

提示信息本身很清楚，但模板默认已带 `/healthz`，`modules: ["health"]` 却仍是一个看
起来合理的组合。建议在模板元数据里列出默认路由，或在文档里注明该组合会自动跳过。

---

## 5. libuvcpp：监听 socket 未设置 SO_REUSEADDR

**现象**

进程退出后立刻重启同一端口，`uv_listen` / bind 返回 `EADDRINUSE`（libuv 错误码
-98），而此时 `ss -lptn` 已看不到监听者（处于 TIME_WAIT）：

```
[ERROR] 启动失败，libuv 错误码 -98（address already in use）
molink-core: failed to listen on 127.0.0.1:21118 (libuv error -98)
```

开发期频繁重启、壳层崩溃后自动拉起都会踩到。建议 `uvcpp_web_app` 在 bind 之前对
socket 设置 `SO_REUSEADDR`（Linux/macOS；Windows 上 libuv 已默认处理）。

本项目未绕过，只在文档里注明“需等待 TIME_WAIT 或换端口”。

---

## 6. libuvcpp：WS 升级没有 pre-upgrade 鉴权钩子（设计讨论，不是缺陷）

`uvcpp_web_app::websocket()` 在 101 写完之后才调用用户处理器，因此无法在升级阶段拒
绝未授权连接。本项目改为应用层 `hello` + 5 秒超时 + `1008` 关闭，工作正常；如果未
来希望更早拒绝（减少资源占用），可以考虑一个返回 bool 的 `on_upgrade` 钩子。

---

## 可直接粘贴的英文摘要

> **uvp_build returns a schema-validation error although the build succeeds.**
> Calling `uvp_build` on a generated workspace returns
> `Structured content does not match the tool's output schema: data must NOT have additional properties`,
> yet `build/molink-core` and `build/molink-core-protocol-test` are produced. No
> diagnostics are reachable through the tool. Suggest validating/clamping the
> result rather than discarding it.
>
> **libuvcpp installs a shared library with no RUNPATH.**
> `libuvcpp.so` NEEDS `libllhttp.so.9.2` from the same prefix but has no RPATH.
> An executable linked against it fails at runtime with
> `error while loading shared libraries: libllhttp.so.9.2`, because `DT_RUNPATH`
> is not inherited by transitive dependencies. Set `INSTALL_RPATH "$ORIGIN"`
> (`@loader_path` on macOS) on the installed shared libraries.
>
> **uvp_verify should accept `run.env`.** `uvp_run` has it; `uvp_verify` does not,
> so a service that takes a secret from the environment cannot be verified with
> an authenticated request.
>
> **libuvcpp's listening socket has no SO_REUSEADDR.** Restarting the service on
> the same port immediately after exit fails with `EADDRINUSE` (libuv -98) while
> `ss` shows no listener (TIME_WAIT). Set `SO_REUSEADDR` before bind on
> Linux/macOS.

---

## 7. libuvcpp：webapp 在 Win7/MinGW 下编译失败（可修复的兼容缺陷）

`UVCPP_FEATURES=full`（web + WebApp）、`UVCPP_WINVER=0x0601`、MinGW-w64 GCC
8.1/13.2 下，`src/webapp/uvcpp_log_console.cpp` 编译失败：

```text
uvcpp_log_console.cpp:86:15: error: 'ENABLE_VIRTUAL_TERMINAL_PROCESSING' was not declared in this scope
```

该常量（`0x0004`）在 MinGW 头文件中只在较新的 `_WIN32_WINNT` 下定义。Win7
目标应当允许它缺省：

```cpp
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
```

`SetConsoleMode` 在不支持的控制台上会失败，而 `detect_color_support()` 已经把
失败当作“无颜色”，所以这是一个安全回退。本项目在本地 kit 中打了这个补丁；
建议合入 `src/webapp/uvcpp_log_console.cpp`，否则官方 Win7 x86 的 full/webapp
profile 无法用 MinGW 构建。

另外，kit 自带的 GCC 8.1 的 `<filesystem>` 在处理 `wstring` 路径和 `path`
比较时会触发 libstdc++ 8 的已知问题（`__is_encoded_char` 实例化后特化、
`u8path` 等）。moLink 的 Win7 构建改用 GCC 13.2 i686（msvcrt），不是
libuvcpp 本身的缺陷，但 kit 文档可以提示“使用 full/webapp 时建议 GCC 9+”。
