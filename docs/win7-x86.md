# Windows 7 x86（32 位）客户端构建

目标：在 Windows 7 SP1 x86 上运行 moLink 客户端（core + UI），与现有 64 位
Windows 客户端使用同一套协议和目录。

## 依赖与工具链

- **libuvcpp Win7 x86 kit**：`uvcpp-kit-1.8.3-mingw32.zip`
  （[libuvcpp releases](https://github.com/Antruly/libuvcpp/releases)）。
  kit 内含 libuv 1.40、OpenSSL 1.1.1w、zlib、llhttp 和一套 GCC 8.1。
- **编译器**：kit 自带的 GCC 8.1 **不能**用于 moLink（见下方已知问题），改用
  [niXman mingw-builds](https://github.com/niXman/mingw-builds-binaries)
  的 `i686-13.2.0-release-posix-dwarf-msvcrt`（msvcrt 运行时，Win7 友好）。
- `nlohmann/json` 头文件：可以复用 `deps/nlohmann_json` 源码树，或把
  `nlohmann/` 放进 libuvcpp 的安装前缀。

## 构建 libuvcpp（full profile）

full = web + WebApp（moLink 需要 WebApp；kit 的 `prebuilt/` 只是 web profile）。

```powershell
$KIT = 'C:\path\to\libuvcpp-1.8.3-mingw32'
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
$KIT = 'C:\path\to\libuvcpp-1.8.3-mingw32'
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
  -Kit C:\path\libuvcpp-1.8.3-mingw32 `
  -Gcc C:\path\i686-13.2.0-posix-dwarf-msvcrt `
  -WebView2 C:\path\wv2 -Version 0.8.1 -Out build\package
```

`.github/workflows/release-win7.yml` 在 `win7-x86-v*` 标签或手动触发时下载
libuvcpp kit、GCC 13、nlohmann/json 和 WebView2 SDK，调用该脚本并上传 zip 与
安装器；标签构建还会创建对应的 Release。

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
