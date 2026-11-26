# moLink Host（被控端专用版）

`moLink Host` 是只做“被控”的 Windows 安装包：装上之后本机就能被远程控制，
界面上没有“去连接别人”的入口。它和全功能版 `moLink` 是两个独立产品，可以装在同一台机器上，
但共享用户配置这一项目前还没有隔离（见 [与全功能版并存](#与全功能版并存)）。

- 安装目录：`C:\Program Files\moLinkHost`（`{autopf}\moLinkHost`）
- 服务名：`moLinkHost`
- 服务数据：`%ProgramData%\moLinkHost\service`
- 安装包：`moLinkHost-<version>-x64.exe`
- 主程序：`moLink-host.exe`（Direct2D 1.0 极简窗口，不依赖 WebView2）

## 组成

安装包由 `packaging/windows/build-host-only.ps1` 收集、`packaging/windows/host-only.iss` 打包：

| 文件 | 作用 |
| --- | --- |
| `moLink-host.exe` | 极简被控端 UI：状态、验证码、网络、安全、服务与自启 |
| `molink-core.exe` | 采集/输入/传输/会话核心，和全功能版使用同一套协议 |
| `molink-service.exe` | 把 `moLinkHost` 注册为 Windows 服务 |
| `molink-helper.exe` | 服务在登录会话中托管 core（含安全桌面支持） |
| `web\` | Web 客户端产物，供 `http://127.0.0.1:<port>/` 使用 |
| `molink.ico` | 安装器与快捷方式图标 |
| 其余 `*.dll` | MSVC 运行库、libuvcpp、OpenSSL、vcpkg 编解码/WebRTC 依赖 |

`molink-helper.exe` 不是可选的：服务本身只负责启动 helper，helper 才负责拉起并守护
`molink-core.exe`。没有 helper 时服务能安装成功，但不会有可被远程的 core。

## 构建

### 前置条件

CI 或本地已经完成：

1. `backend/build-host-only/Release/molink-core.exe`，且该构建目录配置时带
   `-DMOLINK_HOST_ONLY_BUILD=ON`（见下一节）
2. `shell/windows/build/Release/moLink-host.exe`
3. `shell/windows/service/build/Release/molink-service.exe`
4. `shell/windows/helper/build/Release/molink-helper.exe`
5. `frontend/dist/index.html`

### host_only 默认值（构建期）

被控端专用版的 core 必须在**服务端**就拒绝出站会话，不能只靠 UI 隐藏“连接设备”
表单。默认值在 `backend/src/settings.h` 的 `default_host_only()` 里编译进来：

- 全功能版默认构建：`host_only=false`（行为不变）；
- 被控端专用 core：configure 时加 `-DMOLINK_HOST_ONLY_BUILD=ON`，默认
  `host_only=true`。

这是一个**默认值、不是编译期锁**：设置页 / `POST /api/settings?host_only=0`
仍然可以关掉，关掉后会持久化。为了让升级安装也立即生效，host-only 构建第一次
读取旧 `settings` 文件（没有 `host_only_build` 标记文件）时按默认值强制开启；
它自己保存过一次设置后，文件里的值就是用户的最终选择。该标记只由 host-only
构建写入，全功能版完全忽略。

```bash
cmake -S backend -B backend/build-host-only \
  -DMOLINK_HOST_ONLY_BUILD=ON \
  -DMOLINK_UVCPP_PREFIX="<libuvcpp 前缀>"
cmake --build backend/build-host-only --config Release --parallel
```

全功能版的 `backend/build` 不需要（也不应该）带这个选项。

### 生成 stage

```powershell
./packaging/windows/build-host-only.ps1 `
  -Version 0.12.24 `
  -Arch x64 `
  -CoreBuild .\backend\build-host-only `
  -Stage .\build\host-only\stage
```

脚本会：

- 检查 `-CoreBuild` 的 `CMakeCache.txt` 里有
  `MOLINK_HOST_ONLY_BUILD:BOOL=ON`，否则拒绝打包（防止把全功能版 core 打进
  host-only 包）；
- 清空并重建 `-Stage`（如果它包含仓库根目录则拒绝执行，避免误删）；
- 复制上面四个 exe、`molink.ico` 和 `web\`；
- 从 build 目录、libuvcpp 前缀、vcpkg、OpenSSL 和最新的 MSVC redist 收集运行时 DLL；
- 校验 `moLink-host.exe` / `molink-core.exe` / `molink-service.exe` / `molink-helper.exe` /
  `molink.ico` / `web\index.html` / `msvcp140.dll` / `vcruntime140.dll` 都存在，否则失败。

可用参数：`-Repo`、`-CoreBuild`、`-ShellBuild`、`-ServiceBuild`、`-HelperBuild`、
`-FrontendDist`、`-UvcppRoot`、`-VcpkgRoot`。`-CoreBuild` 默认是
`backend\build-host-only`（全功能版用 `backend\build`，两者不能混用）；其余默认值对应 CI 的构建目录；
libuvcpp 默认取 `$env:UVCPP_ROOT` 或 `$env:RUNNER_TEMP\uvcpp`，
vcpkg 默认取 `$env:VCPKG_INSTALLATION_ROOT`。

### x86 / Windows 7 现状

**x86 构建暂不支持。** `build-host-only.ps1 -Arch x86` 会直接报错退出，
不会产出一个缺 core 或缺 shell 的半成品。原因：

- `moLink-host.exe` 目前只有 MSVC x64 目标；
- Win7 x86 需要走现有的 GCC 13 + libuvcpp 1.8.5 MinGW kit（或等价的 GDI/D2D 兼容构建），
  这条链路还没有接到 host-only 产品上；
- 安装脚本里的 `MinVersion=6.1sp1`、Direct2D 1.0 和不使用 8.1+ API 是为将来的
  Win7 包准备的。当前 x64 包的核心仍是 MSVC 构建，核心里有 Win8+ 的导入
  （如 `CreateFile2`），所以**当前的 x64 包不要在 Windows 7 上运行**；
  真正的 Win7 包要等 x86 构建接通后用 x86 产物验证。

### 用 Inno Setup 出安装包

```bat
ISCC.exe /DAppVersion=0.12.24 ^
         /DPkgDir=C:\path\to\build\host-only\stage ^
         /DOutputDir=C:\path\to\build\host-only\package ^
         /DLangFile=packaging\windows\ChineseSimplified.isl ^
         /DOutputName=moLinkHost-0.12.24-x64 ^
         packaging\windows\host-only.iss
```

产物：`moLinkHost-0.12.24-x64.exe`。

CI 的 `windows` job 里顺序是：编译 core/shell/service/helper/frontend →
`Stage the host-only package` → `Build the host-only installer`（ISCC）→
`upload-artifact`。产物名单独使用 `moLinkHost-<ver>-x64.exe`，artifact 名为
`moLink-windows-host-only`，不使用全功能版的 `moLink-*setup-x64` 名称。
release job 按 `moLink-*` 收集 artifact，所以这个新 artifact 会自动进入发布资产。

### Linux 上的静态检查

Inno Setup 不能在 Linux 上编译，CI 的 `shell` job 用
`tools/check-inno-script.py` 检查：

```bash
python3 tools/check-inno-script.py packaging/windows/host-only.iss
```

它校验：

- `host-only.iss` 的 `HostPayload` / `HostWebDir` 与 `build-host-only.ps1` 的列表一致；
- 卸载段先 `sc stop moLinkHost`、再 `molink-service.exe --uninstall --name moLinkHost`；
- `AppId` 与全功能版不同，且不引用 `moLinkSession`；
- 服务安装后设置为 `delayed-auto`（不是 `demand`）；
- 卸载说明包含保留 `%ProgramData%\moLinkHost`，且脚本里没有删除该目录的动作；
- `-Arch x86` 会明确失败。

## 安装行为

安装器需要管理员权限。任务选项：

- **安装为系统服务（关窗口后仍可被远程）**（默认勾选）
  - 运行 `molink-service.exe --install --name moLinkHost`；
  - 服务把 `molink-service.exe`、`molink-helper.exe` 复制到
    `%ProgramData%\moLinkHost\service`，记录 `core.path`（指向安装目录的
    `molink-core.exe`），创建并立即启动服务；
  - 再执行 `sc config moLinkHost start= delayed-auto` 和 `sc start moLinkHost`。
    **必须是自动启动**：无人登录时也要能启动 helper/core；`demand` 只能等有人来
    `sc start`，不满足“开机即可被控”。
- **开机自动启动界面**（默认不勾选）
  - 写 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 的 `moLinkHost` 值，
    指向 `"…\moLink-host.exe"`；
  - 已安装服务时不需要它：服务负责 core，UI 只在需要看状态时打开；
  - UI 自己的“开机自启”勾选框写同一个值，不会产生重复项。
- **桌面快捷方式**（默认不勾选）。
- **防火墙放行**（默认勾选）：规则名 `moLink Host`，放行
  `{app}\molink-core.exe`，所有网络配置。规则名与全功能版的 `moLink` /
  `moLink Core` 不同，互不覆盖。

安装完成后会启动 `moLink-host.exe`。服务安装失败不会回滚文件；安装器隐藏了
`molink-service.exe` 的 stderr，排查时请用管理员命令行手动执行
`molink-service.exe --install --name moLinkHost` 查看错误阶段。服务成功启动后的
日志在 `%ProgramData%\moLinkHost\service\service.log` / `helper.log`。

## 卸载行为

按顺序：

1. 结束 `moLink-host.exe`（UI）；
2. `sc stop moLinkHost`；
3. `molink-service.exe --uninstall --name moLinkHost`
   （内部会停止服务并删除服务注册、`molink-service.exe`、`molink-helper.exe`）；
4. 从 `%ProgramData%\moLinkHost\service\core.pid` 读出服务托管的 core PID，
   按 `PID + IMAGENAME=molink-core.exe` 精确结束它（**不会**按镜像名杀掉
   全功能版的 core）；
5. 删除防火墙规则 `moLink Host`；
6. 等待约 2 秒，让 UI 自己拉起的 core 通过 `--parent-pid` 感知父进程退出，
   然后再删除安装目录文件。

**保留** `%ProgramData%\moLinkHost`（服务/helper 日志、`core.pid`、`core.path`、
`core.port` 等）。
卸载向导会明确提示这一点；如需彻底清除，卸载后手动删除该目录。
`%APPDATA%\moLink`（用户配置/身份）不属于 host-only 产品独占，不会被卸载器删除。

## 服务与自启

| 场景 | 行为 |
| --- | --- |
| 已安装服务 + 关闭窗口 | 窗口收进托盘；服务继续运行，仍可被远程 |
| 已安装服务 + 托盘“退出” | 只退出 UI，提示“服务仍在后台运行” |
| 未安装服务 + 关闭窗口 | 结束**本 UI 拉起**的 core 并退出；被服务或其他进程拉起的 core 不会被 UI 结束 |
| 最小化 | 收进托盘 |
| 开机 | 服务 `delayed-auto` 启动 helper/core；UI 是否自启由 Run 值决定 |

服务相关的 SCM 名和命名对象都由服务名派生（`shell/windows/service/service_paths.*`）：

- `moLinkHost` → `%ProgramData%\moLinkHost\service`，
  `Global\moLinkHostCoreSupervisor`、`Global\moLinkHostHelperStop`、
  `Global\moLinkHostHelperDetach`；
- 全功能版 `moLinkSession` 保持原来的 `%ProgramData%\moLink\service` 和
  `Global\moLink*` 名称，升级不会迁移旧目录。

## 与全功能版并存

已经隔离的：

| 项目 | moLink Host | 全功能版 |
| --- | --- | --- |
| 安装目录 | `C:\Program Files\moLinkHost` | `C:\Program Files\moLink` |
| Inno `AppId` | 独立 GUID | `{B2D9F7D4-…}` |
| 服务名 | `moLinkHost` | `moLinkSession` |
| 服务数据/日志 | `%ProgramData%\moLinkHost\service` | `%ProgramData%\moLink\service` |
| core 网页端口 | 28419–28430 中第一个可用端口，写入 `core.port` | 28418 |
| 服务命名对象 | `Global\moLinkHost*` | `Global\moLink*` |
| UI 单实例互斥量 | `moLinkHostShell` | 全功能壳自己的互斥量 |
| Run 值 / 防火墙规则 | `moLinkHost` / `moLink Host` | 全功能版自己的名称 |

需要注意的：

- **用户配置目录目前仍共用** `%APPDATA%\moLink`（`config_directory()` 没有按产品名隔离）。
  同一 Windows 账号下，两套产品会看到同一个 `identity`、`settings`。服务的 core
  不再依赖共享的 `tcp_port`：下一条的端口由 helper 显式传入。
- **端口已隔离**：`moLinkHost` 的服务 core 不再监听 28418。helper 从
  `service_paths::default_port_for("moLinkHost")` 给出的保留段端口（28419–28430）
  开始，用 `bind` 探测最多 5 个端口，把选中的端口通过 `--port` 传给 core，并写入
  `%ProgramData%\moLinkHost\service\core.port`；UI 优先探测这个文件里的端口，不再
  硬编码 28418。全功能版 `moLinkSession` 保持 28418 和原有行为。
- **core 归属**：host-only helper 接管已有 core 前，会用
  `QueryFullProcessImageNameW` 取进程完整路径，要求它等于
  `{安装目录}\molink-core.exe`（大小写不敏感）；拿不到路径或路径不属于
  `%ProgramData%\moLinkHost\service\core.path` 指向的安装目录时跳过接管、启动自己的
  core。helper.log 里对应 `core.adopt pid=… path=… own=1|0 action=reuse|spawn`。
  因此全功能版的 core 不会被 host-only 服务接管。
- 仍需要注意：未安装服务时由 UI 自己拉起的 core 走 core 的默认/持久化端口；如果
  同一账号下全功能版正在运行，仍可能冲突。需要完全并存时，优先用服务托管 core。
- 卸载 host-only 不会删除全功能版的服务、文件或 core；卸载全功能版也不会删除
  host-only 的 `%ProgramData%\moLinkHost`。
- 两套产品共用同一份 `molink-core.exe`/协议版本时，建议同版本升级；混用旧 core
  和新的 UI 可能缺少 `/api/state` 字段。

## 排错

- 服务状态：`sc query moLinkHost`；服务日志：
  `%ProgramData%\moLinkHost\service\service.log`、`helper.log`。
- `core.path` 指向服务应当启动的 core；文件不存在时 helper 日志会写
  `molink-core.exe not found`。`core.port` 是 helper 为服务 core 选中的网页端口，
  `core.pid` 是它守护的进程。
- 判断某个 core 是否被本服务接管：helper.log 的
  `core.adopt pid=… path=… own=… action=…`。`own=0` 表示路径不属于本服务安装目录，
  已跳过；`own=1 action=reuse` 才是接管。两个服务并存时应各有一行 `own=1 action=reuse`，
  且 path 分别指向各自的安装目录。
- UI/核心日志：`%LOCALAPPDATA%\moLink\logs`（如果 `%ProgramData%\moLink`
  已存在，则 core 诊断日志在 `%ProgramData%\moLink\logs`）。
- 安装了服务但连不上：确认防火墙规则 `moLink Host` 存在、端口是
  `sc query`/UI 显示的实际端口、并且服务处于 `RUNNING`。
- 手动安装/卸载服务（管理员）：
  ```bat
  molink-service.exe --install --name moLinkHost
  sc config moLinkHost start= delayed-auto
  sc start moLinkHost
  sc stop moLinkHost
  molink-service.exe --uninstall --name moLinkHost
  ```

## 验收清单

- [ ] `python3 tools/check-native-build-lists.py` 通过
- [ ] `python3 tools/check-inno-script.py packaging/windows/host-only.iss` 通过
- [ ] `build-host-only.ps1 -Arch x86` 明确失败
- [ ] CI `windows` job 产出 `moLinkHost-<ver>-x64.exe` 并上传
      `moLink-windows-host-only`
- [ ] 安装后 `sc query moLinkHost` 为 `RUNNING`，未登录时可被远程
- [ ] 卸载后服务消失、`%ProgramData%\moLinkHost` 保留
