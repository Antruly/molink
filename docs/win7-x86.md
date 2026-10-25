# Windows 7 x86（32 位）客户端构建

目标：在 Windows 7 SP1 x86 上运行 moLink 客户端（core + UI），与现有 64 位
Windows 客户端使用同一套协议和目录。

## 依赖与工具链

- **libuvcpp Win7 x86 kit**：`uvcpp-kit-1.8.5-mingw32.zip`
  （[libuvcpp releases](https://github.com/Antruly/libuvcpp/releases)）。
  kit 内含 CMake、libuv 1.40、OpenSSL 1.1.1w、zlib、llhttp、一套 GCC 8.1，
  以及 `prebuilt/` —— 1.8.5 起它就是 **full 档**（`UVCPP_WEBAPP_ENABLE=1`）。
- **编译器**：kit 自带的 GCC 8.1 **不能**用于 moLink（见下方已知问题），改用
  [niXman mingw-builds](https://github.com/niXman/mingw-builds-binaries)
  的 `i686-13.2.0-release-posix-dwarf-msvcrt`（msvcrt 运行时，Win7 友好）。
- 不需要 nlohmann/json 源码树：1.8.5 kit 的 `prebuilt/` 已经是 full 档；
  只有回退到“从源码编 libuvcpp”才用得上它。

## 推荐：直接用 1.8.5 kit（prebuilt 就是 full 档）

`uvcpp-kit-1.8.5-mingw32.zip` 的 `prebuilt/` 就是一个装好的 full 档前缀：
`bin/libuvcpp.dll`、`lib/libuvcpp.dll.a` 和全部头文件，`UVCPP_WEBAPP_ENABLE=1`；
libuv/OpenSSL/llhttp 都静态链进了 `libuvcpp.dll`。脚本在 `-Kit` 指向 1.8.5 kit 且
没传 `-UvcppPrefix` 时**默认用这个 prebuilt**，不再编译 libuvcpp。

也可以改用单独的发布包 `uvcpp-win7-x86-mingw32.zip` 作为前缀（内容与 `prebuilt/` 相同）。

例外是 zlib：molink-core 自己直接调用 `compress2`（`backend/src/compression.cpp`），
而 libuvcpp 的发布包只有 `include/zlib.h`、没有 zlib 库，所以仍从 **kit 的 `deps/zlib`**
取 `libzlibstatic.a`（纯 C，ABI 无关），并传 `ZLIB_LIBRARY` / `ZLIB_INCLUDE_DIR`。
OpenSSL 不需要，客户端没有直接调用。

`packaging/windows/build-win7-x86.ps1 -Kit <1.8.5 kit> [-UvcppPrefix <发布包>] ...`
封装了这条路径；不传 `-UvcppPrefix` 就用 kit 的 `prebuilt/`。

## 从源码构建 libuvcpp（只有 1.8.5 之前的旧 kit 需要）

旧 kit 的 `prebuilt/` 只有 web 档（没有 WebApp），这时才需要自己编一份 full。

```powershell
$KIT = 'C:\path\to\libuvcpp-<1.8.5 之前的 kit>'
$GCC = 'C:\path\to\i686-13.2.0-posix-dwarf-msvcrt'
cmake -S $KIT -B $KIT\build-gcc13-full -G "MinGW Makefiles" `
  -DCMAKE_C_COMPILER="$GCC/bin/gcc.exe" `
  -DCMAKE_CXX_COMPILER="$GCC/bin/g++.exe" `
  -DCMAKE_RC_COMPILER="$GCC/bin/windres.exe" `
  -DCMAKE_BUILD_TYPE=Release `
  -DUVCPP_FEATURES=full `
  -DUVCPP_WINVER=0x0601 `
  -DUVCPP_STATIC_RUNTIME=ON `
  -DUVCPP_USE_SYSTEM_LIBUV=ON `
  -DLIBUV_INCLUDE_DIR="$KIT/deps/libuv/include" `
  -DLIBUV_LIB="$KIT/deps/libuv/lib/libuv_a.a" `
  -DOPENSSL_ROOT_DIR="$KIT/deps/openssl" -DOPENSSL_USE_STATIC_LIBS=ON `
  -DZLIB_ROOT="$KIT/deps/zlib" `
  -DZLIB_LIBRARY="$KIT/deps/zlib/lib/libzlibstatic.a" `
  -DZLIB_INCLUDE_DIR="$KIT/deps/zlib/include" `
  -DFETCHCONTENT_SOURCE_DIR_LLHTTP="$KIT/deps/llhttp" `
  -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON="$KIT/deps/nlohmann" `
  -DCMAKE_INSTALL_PREFIX="$KIT/out/gcc13"
cmake --build $KIT\build-gcc13-full --parallel 8
cmake --install $KIT\build-gcc13-full
```

`UVCPP_STATIC_RUNTIME=ON` 让 `libuvcpp.dll` 不依赖 libstdc++/libgcc/winpthread。

## 构建 molink-core

仓库已带工具链文件 `cmake/toolchains/mingw32-win7.cmake`（读取
`MOLINK_WIN7_KIT`）。把 kit 的 `out/gcc13` 当作 uvcpp 前缀：

```powershell
$KIT = 'C:\path\to\libuvcpp-<1.8.5 之前的 kit>'
$env:MOLINK_WIN7_KIT = $KIT
$env:MOLINK_WIN7_GCC = 'C:\path\to\i686-13.2.0-posix-dwarf-msvcrt'
# uvcpp 的 CMake target 还引用裸 `uv`，把 import lib 放进前缀
Copy-Item "$KIT\deps\libuv\lib\libuv.dll.a" "$KIT\out\gcc13\lib\"
Copy-Item "$KIT\deps\libuv\bin\libuv.dll" "$KIT\out\gcc13\bin\"
Copy-Item -Recurse "$KIT\deps\libuv\include\*" "$KIT\out\gcc13\include\"

cmake -S backend -B build-win7x86 -G "MinGW Makefiles" `
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw32-win7.cmake `
  -DCMAKE_MAKE_PROGRAM="$KIT/deps/mingw32/bin/mingw32-make.exe" `
  -Duvcpp_DIR="$KIT/out/gcc13/lib/cmake/uvcpp" `
  -DCMAKE_PREFIX_PATH="$KIT/out/gcc13;$KIT/deps/libuv;$KIT/deps/zlib;$KIT/deps/openssl" `
  -DMOLINK_CXX_STANDARD=17 -DCMAKE_BUILD_TYPE=Release `
  -DZLIB_INCLUDE_DIR="$KIT/deps/zlib/include" `
  -DZLIB_LIBRARY="$KIT/deps/zlib/lib/libzlibstatic.a"
cmake --build build-win7x86 --parallel 8
```

工具链文件默认使用 kit 的 GCC 8.1；设置 `MOLINK_WIN7_GCC`（环境变量或
`-DMOLINK_WIN7_GCC=...`）即可切换到 GCC 13。

## 运行

把以下文件放进一个目录：

```
molink-core.exe
libuvcpp.dll
libuv.dll
libgcc_s_dw2-1.dll   # GCC 13 的运行时；若用 -static 链接可省略
libstdc++-6.dll
libwinpthread-1.dll
web/
```

```bat
set MOLINK_CONFIG_DIR=%APPDATA%\moLink
molink-core.exe --host 0.0.0.0 --port 28418 --print-ready
```

目录/证书配置与 64 位客户端一致：`settings` 里的 `server_url`、`server_ca`。
Win7 上如果服务端使用自签证书，需要把 CA PEM 放到本机并在 `server_ca` 指定。

## UI 与安装包

- 现有 `shell/windows` 是 Win32 + WebView2。Win7 需要 **WebView2 Runtime 109**
  （微软最后一个支持 Win7 的版本）或固定版本运行时；标准路径不存在时 shell
  无法启动。
- 没有 WebView2 时，可以先使用 core 托管的 Web UI：
  `allow_web_client=1`，用支持现代 JS 的浏览器打开 `http://127.0.0.1:28418/`。
  Win7 自带 IE11 不支持 React 19，需要 Firefox ESR 115 或 Chrome 109。
- 服务/helper（`molink-service.exe` / `molink-helper.exe`）同样用 i686 工具链
  构建，才能支持锁屏/登录页采集。

## 自动化构建

仓库里的 `packaging/windows/build-win7-x86.ps1` 封装了完整流程（libuvcpp full、
core、shell、service/helper、打包、Inno Setup）：

```powershell
./packaging/windows/build-win7-x86.ps1 `
  -Kit C:\path\libuvcpp-1.8.5-mingw32 `
  -Gcc C:\path\i686-13.2.0-posix-dwarf-msvcrt `
  -WebView2 C:\path\wv2 -Version 0.9.3 -Out build\package
```

`.github/workflows/release-desktop.yml` 的 `win7-x86` job 在 `v*` 标签时下载
**1.8.5 kit**、GCC 13 和 WebView2 SDK（不再单独取发布包，也不需要 nlohmann/json），
调用该脚本并上传 zip 与安装器；资产附加到同一个 Release。

## 已验证状态（2026-10-25）

- Win7 控制台会话：GDI 采集 1024×768、SendInput、32×32 RGBA 光标与
  `cursor-position` 消息、真实 129 KB 关键帧。
- 目录注册/心跳/域组（TLS）和 ID 解析通过；WS 欢迎与画面帧通过。
- Win7 测试机没有声卡设备，欢迎消息里 `audio=false` 是预期行为。
- 原生 WebView2 外壳已编译；测试机没有 WebView2 Runtime 109，因此用 Firefox
  ESR 115 的浏览器 UI 验证。
- 入站 28418 需要在管理员权限下运行 `install-firewall.cmd` 或使用安装器的
  “允许防火墙”任务。

## 已知问题

1. **kit 自带 GCC 8.1 的 `std::filesystem` 不可用**：`std::filesystem::path` 在
   处理 `wstring` / 比较时触发 libstdc++ 8 的已知缺陷（`__is_encoded_char`
   specialization after instantiation、`u8path` 等）。moLink 必须用 GCC 9+；
   本文使用 GCC 13.2。
2. **libuvcpp webapp 在 Win7/MinGW 下编译失败**：`uvcpp_log_console.cpp` 使用
   `ENABLE_VIRTUAL_TERMINAL_PROCESSING`，MinGW 在 `_WIN32_WINNT=0x0601` 下不定义
   该常量。本地补丁：
   ```cpp
   #ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
   #define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
   #endif
   ```
   已记录在 `docs/upstream-feedback.md`。
3. **Win7 上没有 WebView2**：需要单独安装 109 运行时，或走浏览器 Web UI。
4. **32 位构建的 API 下限**：`_WIN32_WINNT=0x0601`；不要引入仅在 Windows 8+
   可用的 API（如 `GetSystemTimePreciseAsFileTime`），否则 Win7 启动即失败。

## 故障排查

| 现象 | 处理 |
|---|---|
| 原生壳启动后空白/提示 WebView2 | Win7 需要 WebView2 Runtime **109**（最后一个支持 Win7 的版本）；没有时用 `start-browser-ui.cmd` + Firefox ESR 115 / Chrome 109 |
| 浏览器打开后是白屏或脚本错误 | 不要用 IE11；确认设置里 `allow_web_client=1`，并使用 Firefox ESR 115/Chrome 109 |
| 局域网能发现设备但连不上 | 入站 28418 被防火墙挡住；管理员运行 `install-firewall.cmd` 或安装器的防火墙任务（`profile=any`）；确认两台机器在同一网段、没有 NAT 隔离 |
| 远程桌面有画面但没有键鼠 | 核心需要运行在登录用户的交互式会话（桌面/启动项/计划任务），不能是服务会话；查看欢迎消息 `input=true` |
| 锁屏/登录页看不到 | 需要以管理员安装 `molink-service`（安装器勾选“安装安全桌面服务”或运行 `install-service.cmd`）；纯 GDI 桌面采集不覆盖 Winlogon |
| 目录 TLS 失败 | 默认目录是 `https://molink.ssvr.top:28430`，服务器必须在 28430 提供 TLS；自签/私有 CA 要在客户端 `server_ca` 指定 PEM；检查证书 SAN 含域名和系统时间 |
| 域组 tab 提示无此域码 | 域的属主更新了码；向对方要新码。旧版 0.2.0 目录没有域组功能，需要 0.2.1+ |
| 没有声音 | Win7 测试虚拟机没有声卡；欢迎消息 `audio=false` 是预期行为 |
| 在远程/过滤令牌会话里安装服务失败 | `sc create` 需要管理员权限的完整令牌；用安装器（UAC）或在控制台以管理员运行 `install-service.cmd` |
| 启动时报缺少 DLL | 保持 zip/安装目录结构完整；MinGW 运行库（`libstdc++-6`、`libgcc_s_dw2-1`、`libwinpthread-1`）与 `libuvcpp.dll` 必须在 exe 同目录 |
