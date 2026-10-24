# moLink Server 部署与运维

`molink-server` 是 moLink 二阶段的设备目录服务：桌面客户端注册设备、上报心跳，
客户端通过设备 ID 查询地址后直连；同时提供独立的 Web 管理控制台、账号体系、
IP 来源策略、请求限速、临时封禁和审计日志。

服务端版本独立于桌面端：标签为 `server-v*`，当前版本 `0.2.0`。

## 1. 端口与组成

| 监听 | 默认地址 | 用途 |
|---|---|---|
| 设备 API | `0.0.0.0:28430` | `/v1/devices/register|heartbeat|lookup|unregister`、`/healthz` |
| 管理 API/网页 | `127.0.0.1:28431` | `/api/admin/*` 与内置管理控制台 |

管理面默认只绑定回环地址，安装时可选择绑定 `0.0.0.0`。公网部署时建议：

- 管理面保持回环，通过 SSH 隧道或本机浏览器访问；或
- 绑定内网/公网时配置 TLS（`admin.tls_cert/key`）或放在 HTTPS 反向代理后。

设备面与桌面客户端保持兼容：接口路径、参数与返回字段不变，新增的拒绝码包括
`ip_denied`、`ip_banned`、`rate_limited`。

## 2. 安装

### Linux

提供 `.deb`、`.rpm` 与 `.tar.gz`。deb/rpm 安装后会自动创建 `molink-server`
系统用户、数据目录并 `systemctl enable --now`：

```sh
sudo dpkg -i molink-server_0.2.0_amd64.deb
# 或
sudo rpm -i molink-server-0.2.0-1.x86_64.rpm

systemctl status molink-server
journalctl -u molink-server -f
```

tar.gz 解压后以 root 运行：

```sh
./install.sh                 # 安装文件、创建用户、注册并启动 systemd
./install.sh --no-service    # 只安装文件，不注册服务
./install.sh --no-start      # 注册服务但不立即启动
./uninstall.sh               # 保留配置和数据
./uninstall.sh --purge       # 连同配置、数据和服务账户一起删除
```

文件布局：

| 路径 | 内容 |
|---|---|
| `/opt/molink-server/bin/molink-server` | 可执行文件 |
| `/opt/molink-server/lib` | 随包的 libuvcpp/libllhttp |
| `/opt/molink-server/share/web-admin` | 管理控制台静态文件 |
| `/etc/molink-server/config.json` | 配置文件（deb conffile / rpm noreplace） |
| `/var/lib/molink-server` | SQLite 数据库等数据（0700，`molink-server` 用户） |
| `/var/log/molink-server/server.log` | 运行日志（20 MiB × 5 轮转） |

Linux 包依赖系统 `libssl3`、`libuv1`、`zlib1g`；PostgreSQL 客户端库按需安装
（`libpq5`），仅当选择 PostgreSQL 驱动时用到。

### Windows

- 安装器 `moLink-Server-0.2.0-setup-x64.exe`：可选“安装为 Windows 服务”（默认勾选）、
  可选防火墙放行 28430、可选管理面绑定 `0.0.0.0`。配置写入
  `C:\ProgramData\moLink\Server\config.json`，卸载时保留数据。
- 便携 zip：解压到固定目录后运行 `molink-server.exe --config config.json`；
  以管理员运行 `install-service.cmd` 注册服务。

服务名 `moLinkServer`，自动（延迟）启动；日志与数据库默认在
`C:\ProgramData\moLink\Server`。包内尽可能附带 OpenSSL 与 VC++ 运行库；
若目标机器提示缺少 `VCRUNTIME140`/`MSVCP140`，安装 Microsoft Visual C++
Redistributable 即可。

### macOS

- `moLink-Server-0.2.0-arm64.pkg` / `x64.pkg`（未签名）与对应的
  `macos-<arch>.tar.gz`。
- pkg 安装到 `/usr/local/molink-server`，创建 `_molink` 用户，注册
  `/Library/LaunchDaemons/com.molink.server.plist` 并启动。
- 配置 `/usr/local/etc/molink-server/config.json`，数据
  `/usr/local/var/molink-server`，日志 `/usr/local/var/log/molink-server`。

未签名 pkg 受 Gatekeeper 限制：可右键“打开”，或在终端执行
`xattr -d com.apple.quarantine <pkg>`。正式分发需要 Developer ID 与公证，后续补。

服务管理：

```sh
sudo launchctl kickstart -k system/com.molink.server
sudo launchctl bootout system/com.molink.server
```

## 3. 首次使用

浏览器打开 `http://127.0.0.1:28431`：

1. 设置管理员用户名、显示名称和密码（至少 10 个字符）。
2. 创建后进入仪表盘，显示在线/注册设备、账户、封禁、运行时间与监听信息。
3. 左侧菜单：仪表盘、设备、管理员、安全策略、封禁、审计、设置。

如果管理面绑定在非回环地址且还没有管理员，服务端会在启动日志中打印一次性
初始化令牌；在初始化表单中填入即可。也可以通过 `--setup-token` 固定该令牌。

账户角色：

| 角色 | 权限 |
|---|---|
| admin | 全部，包括账户、安全策略和服务设置 |
| operator | 查看、吊销设备、管理封禁、查看审计 |
| viewer | 只读 |

## 4. 配置

优先级：**命令行 > 配置文件 > 环境变量 > 内置默认值**。配置文件为 JSON，
通过 `--config` 指定；环境变量 `MOLINK_SERVER_CONFIG` 也可给出路径。

```json
{
  "server": {
    "data_dir": "/var/lib/molink-server",
    "registration_token": "",
    "public_url": "",
    "offline_after_seconds": 90
  },
  "device": {
    "host": "0.0.0.0", "port": 28430,
    "tls_cert": "", "tls_key": ""
  },
  "admin": {
    "host": "127.0.0.1", "port": 28431,
    "tls_cert": "", "tls_key": "",
    "web_root": "/opt/molink-server/share/web-admin",
    "allow_insecure_remote": false
  },
  "database": {
    "driver": "sqlite", "url": "",
    "pool_min": 1, "pool_max": 8, "busy_timeout_ms": 5000
  },
  "log": {
    "level": "info", "file": "",
    "access_log": false, "max_size_mb": 20, "max_files": 5
  }
}
```

常用命令行参数：

```
--config <file>  --host/--port  --tls-cert/--tls-key
--admin-host/--admin-port  --admin-tls-cert/--admin-tls-key
--ui-dir <dir>  --data-dir <dir>
--db-driver sqlite|postgres  --db-url <url>  --db-pool-min/--db-pool-max
--registration-token <t>  --public-url <url>  --offline-after <seconds>
--setup-token <t>  --allow-insecure-admin
--log-level debug|info|warn|error  --log-file <path>
--print-ready
```

环境变量：`MOLINK_SERVER_DATA_DIR`、`MOLINK_SERVER_CONFIG`、`MOLINK_SERVER_HOST`、
`MOLINK_SERVER_PORT`、`MOLINK_SERVER_ADMIN_HOST`、`MOLINK_SERVER_ADMIN_PORT`、
`MOLINK_SERVER_REGISTRATION_TOKEN`、`MOLINK_SERVER_PUBLIC_URL`、
`MOLINK_SERVER_DB_DRIVER`、`MOLINK_SERVER_DB_URL`、`MOLINK_SERVER_DB_POOL_MIN`、
`MOLINK_SERVER_DB_POOL_MAX`、`MOLINK_SERVER_DB_BUSY_TIMEOUT_MS`、
`MOLINK_SERVER_LOG_LEVEL`、`MOLINK_SERVER_LOG_FILE`、`MOLINK_SERVER_UI_DIR`、
`MOLINK_SERVER_SETUP_TOKEN`、`MOLINK_SERVER_ADMIN_ALLOW_INSECURE`。

管理页里的「安全策略」和「服务目录」会写回数据库并即时生效；监听地址、端口、
TLS、数据库连接和 UI 目录属于启动配置，修改后需要重启服务。

## 5. 数据库

### SQLite（默认）

`database.driver = "sqlite"`，`url` 留空时使用 `<data_dir>/molink.db`。服务端使用
WAL、`busy_timeout` 和连接池；数据库必须放在本地磁盘（不要在 NFS/SMB 上运行
WAL）。备份：

```sh
sqlite3 /var/lib/molink-server/molink.db ".backup '/var/backups/molink-$(date +%F).db'"
```

### PostgreSQL（可选）

```json
"database": {
  "driver": "postgres",
  "url": "postgres://molink:secret@127.0.0.1:5432/molink",
  "pool_min": 1,
  "pool_max": 8,
  "busy_timeout_ms": 5000
}
```

- 目标机器需要安装 PostgreSQL 客户端库（`libpq`）；服务端通过运行时加载使用它，
  没有安装时只有 PostgreSQL 连接会失败，SQLite 不受影响。
- 可用 `MOLINK_SERVER_DB_URL` 把连接串放进 systemd 的 `EnvironmentFile`，避免
  把密码写进配置文件。
- 表结构与迁移由服务端在启动时自动创建；升级前请先 `pg_dump`。
- 连接串中的密码不会写入日志或管理 API。

## 6. 安全默认值

| 项目 | 默认 |
|---|---|
| IP 来源 | 私有网段 + 中国大陆（APNIC IPv4 表）；拒绝海外 |
| 自定义拒绝/允许 | 空；拒绝优先于允许 |
| 设备 API 限速 | 600 次/分钟/IP，突发 120 |
| 注册 | 10 次/小时/IP，突发 3；每 IP 最多 50 台设备 |
| 心跳 | 6 次/分钟/设备；240 次/分钟/IP |
| 查询 | 60 次/分钟/IP |
| 管理 API | 300 次/分钟/会话 |
| 登录失败 | 5 次触发阶梯 300/1800/21600/86400 秒，自动临时封禁 |
| 管理会话 | 空闲 30 分钟，绝对 12 小时；HttpOnly + SameSite=Strict |
| 密码 | PBKDF2-HMAC-SHA256，600000 次，16 字节盐 |

IPv6：私有/回环地址按私有处理，公网 IPv6 归入“海外”；v1 不内置中国 IPv6 表。

### 反向代理

管理面放在 nginx/Caddy 后时：

1. 管理面绑 `127.0.0.1`，由代理转发到本地端口。
2. 在「安全策略 → 受信代理」加入代理地址 CIDR，否则 `X-Forwarded-For` 不会被采用。
3. 若代理终止 TLS，把 `secure_cookie` 设为 `auto` 或 `always`，并保留
   `trust_forwarded_proto` 开启，以便 Secure Cookie 与 Origin 校验正确。

示例（nginx）：

```nginx
location / {
    proxy_pass http://127.0.0.1:28431;
    proxy_set_header Host $host;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    proxy_set_header X-Forwarded-Proto $scheme;
}
```

设备 API 也可以按同样方式放在代理后；客户端填写代理的 `https://` 地址即可。

## 7. TLS

```sh
molink-server --tls-cert /etc/molink-server/device.crt \
              --tls-key  /etc/molink-server/device.key \
              --admin-tls-cert /etc/molink-server/admin.crt \
              --admin-tls-key  /etc/molink-server/admin.key
```

设备面与管理面可以使用不同证书。TLS 上目前只宣告 HTTP/1.1。管理面绑定非回环
且未启用 TLS 时启动会告警；显式允许需设置 `allow_insecure_remote` 或
`--allow-insecure-admin`。

## 8. 权限与首次管理员

- 管理员的密码哈希保存在数据库；数据库文件权限由 systemd `StateDirectory`
  保证为 `molink-server` 用户所有。
- 不支持默认密码；没有用户时只能通过初始化表单或令牌创建第一个管理员。
- 忘记密码：在数据库里删除 `admin_users` 行会重新开放初始化（会丢失账户）；
  更安全的做法是备份数据库后使用 SQL 重置。请先阅读 `devices.json` 旧数据导入
  规则。

## 9. 健康检查与日志

- `GET /healthz`（设备面）与管理面 `/healthz` 返回服务版本与数据库状态。
- 管理页仪表盘显示运行时间、在线设备、数据库 schema 版本。
- 日志默认写 stderr（systemd 收集到 journald）；配置文件/安装器会把日志写入
  文件并按大小轮转（默认 20 MiB × 5）。

## 10. 升级

1. 备份数据库（SQLite 复制/`.backup`，PostgreSQL `pg_dump`）。
2. 停止服务，替换二进制与 `web-admin`，保持 `/etc` 与 `/var/lib` 不变。
3. 启动；服务端会在事务中执行新的 schema 迁移。
4. 检查 `/healthz` 与管理页概览。

旧版（0.1.x）的 `devices.json` 目录会在首次启动时导入到数据库；导入后文件会
改名为 `devices.json.imported.<时间戳>`，保留原文件以便回退。

## 11. 故障排查

| 现象 | 处理 |
|---|---|
| 设备注册返回 403 `ip_denied` | 来源不在私有/中国/自定义允许范围；调整安全策略或加代理 |
| 403 `ip_banned` | 在「封禁」中解除，或等待自动封禁到期 |
| 429 `rate_limited` | 查看响应 `Retry-After`；检查安全策略中的限速 |
| 登录 403 `csrf_failed` | 浏览器时间/代理导致 Cookie 与请求头不一致；刷新页面重新登录 |
| 管理页 401 | 会话过期或服务重启；重新登录 |
| 初始化需要令牌 | 查看服务启动日志中的一次性 token，或使用 `--setup-token` |
| Windows 服务起不来 | 查看 `C:\ProgramData\moLink\Server\server.log`；确认配置文件路径 |
| PostgreSQL `NO_DRIVER` | 目标机器缺少 libpq；安装客户端库或改用 SQLite |
| macOS pkg 被拦截 | 右键打开 / 去除 quarantine；正式签名后续提供 |

## 12. 规模与限制

- 设计目标为单机自托管、约一万台设备；SQLite 足够；更大规模建议 PostgreSQL。
- 目录只负责寻址，远程会话仍需要对方验证码或人工批准；目前没有中继。
- 分布式/多节点管理属于第三阶段（Pro），见
  [server-distributed.md](server-distributed.md)。
