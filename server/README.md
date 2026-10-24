# moLink Server

moLink 的设备目录与自托管管理服务。当前版本 **0.2.0**，独立于桌面端
（`server-v*` 标签）。

## 能力

- 设备注册、30 秒心跳、按 ID 查询地址、注销；协议与桌面客户端 0.8.x 兼容。
- Web 管理控制台：首次初始化、仪表盘、设备、管理员、安全策略、封禁、审计、设置。
- 账号角色（admin / operator / viewer）、HttpOnly 会话 + CSRF、登录失败阶梯。
- IP 来源策略（私有 / 中国大陆 / 海外 + 自定义 CIDR），默认仅私有 + 中国大陆。
- 请求限速、临时封禁、可信代理 `X-Forwarded-For`、审计日志。
- 存储默认 SQLite，可选 PostgreSQL；启动时事务化迁移。
- 设备 API 与管理面分端口：`0.0.0.0:28430` 与 `127.0.0.1:28431`。

## 构建

需要 libuvcpp（WEB/WEBAPP + DB 模块 + OpenSSL）。发布构建使用：

```sh
cmake -S <libuvcpp> -B <build> \
  -DUVCPP_BUILD_WEB=ON -DUVCPP_BUILD_WEBAPP=ON \
  -DUVCPP_ENABLE_OPENSSL=ON \
  -DUVCPP_ENABLE_DB=ON -DUVCPP_ENABLE_DB_SQLITE=ON \
  -DUVCPP_DB_SQLITE_FROM_SOURCE=ON \
  -DUVCPP_ENABLE_DB_PGSQL=ON -DUVCPP_DB_RUNTIME_LOAD=ON \
  -DUVCPP_ENABLE_DB_MYSQL=OFF -DUVCPP_ENABLE_DB_ODBC=OFF \
  -DUVCPP_ENABLE_DB_MONGODB=OFF \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local/uvcpp"
cmake --build <build> --parallel && cmake --install <build>

cmake -S server -B server/build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$HOME/.local/uvcpp" -DBUILD_TESTING=ON
cmake --build server/build --parallel
ctest --test-dir server/build --output-on-failure

cd server/ui && pnpm install --frozen-lockfile && pnpm build
```

## 运行

```sh
./server/build/molink-server --data-dir /tmp/molink-server-data
# 管理控制台: http://127.0.0.1:28431
# 设备 API:   http://127.0.0.1:28430
```

首次打开管理页会要求创建管理员。`--help` 列出全部参数。

## 文档与打包

- 部署、配置、安全、数据库、反向代理与故障排查：[docs/server.md](../docs/server.md)
- 第三阶段分布式设计：[docs/server-distributed.md](../docs/server-distributed.md)
- Linux 安装包：`packaging/linux/build-server-{deb,rpm,tar}.sh`
- Windows 安装器与 zip：`packaging/windows/server-{installer.iss,stage.sh}`、`server-zip.ps1`
- macOS pkg/tar：`packaging/macos/build-server-{pkg,tar}.sh`
- 发布：推送 `server-v*` 标签触发 `.github/workflows/release-server.yml`
