# moLink Server（二阶段）

设备目录与在线状态服务。桌面端启动时注册，之后靠心跳保持在线；任何客户端可以
用设备 ID 查到对方的地址和端口，随后**直连**建立会话。第一阶段媒体流不经过这里；
后续可以在同一注册 API 上增加中继 / TURN。

## 构建与运行

```sh
cmake -S server -B server/build -DCMAKE_PREFIX_PATH="$HOME/.local/uvcpp-uvscope"
cmake --build server/build --parallel
./server/build/molink-server --host 0.0.0.0 --port 28430 \
  --data-dir /var/lib/molink-server
```

参数：

| 参数 | 默认值 | 说明 |
|---|---|---|
| `--host` | `0.0.0.0` | 监听地址 |
| `--port` | `28430` | HTTP 端口 |
| `--data-dir` | `./molink-server-data` | `devices.json` 的目录 |
| `--registration-token` | 空（开放） | 设置后注册必须带 `invite=<值>` |
| `--public-url` | 空 | 返回给客户端的对外 URL |
| `--offline-after` | `90` | 超过多少秒没有心跳视为离线 |

设备目录保存在 `devices.json`（先写临时文件再原子替换）。服务重启后设备仍然存在，
但离线状态由最后一次心跳计算。

## API

所有接口都是查询参数 + JSON 响应，允许跨域（`Access-Control-Allow-Origin: *`）。

### 注册

```sh
curl -X POST 'http://127.0.0.1:28430/v1/devices/register?name=OfficePC&port=28418&version=0.7.0'
```

```json
{"ok":true,"device_id":"9A937NDEZ","display_id":"9A9-37N-DEZ",
 "token":"52f13b46...","server_time":1792843441194,"public_url":""}
```

`token` 只返回一次，设备需要保存；心跳和注销用它鉴权。每个来源地址每小时最多注册
20 台设备。如果服务端设置了 `--registration-token`，还要带 `invite=<值>`。

### 心跳（每 30 秒）

```sh
curl -X POST 'http://127.0.0.1:28430/v1/devices/heartbeat?id=9A937NDEZ&token=<token>&port=28418&version=0.7.0'
```

返回 `204`。来源地址或端口变化会同步更新。

### 查询

```sh
curl 'http://127.0.0.1:28430/v1/devices?id=9A9-37N-DEZ'
```

```json
{"id":"9A937NDEZ","display_id":"9A9-37N-DEZ","name":"OfficePC","online":true,
 "address":"192.168.1.10","peer_ip":"203.0.113.5","port":28418,
 "version":"0.7.0","last_seen_ms":1792843441194,"public_url":""}
```

`address` 是设备自报的地址（局域网内可用），`peer_ip` 是服务端看到的来源地址。
不会返回设备 token。未知设备返回 `404 unknown_device`。

### 注销

```sh
curl -X POST 'http://127.0.0.1:28430/v1/devices/unregister?id=9A937NDEZ&token=<token>'
```

### 健康检查

```sh
curl 'http://127.0.0.1:28430/healthz'
```

## 安全边界（v1）

- 目录只负责寻址；会话仍然需要对方验证码或人工批准。
- 当前版本建议由反向代理提供 HTTPS（`molink-server` 本身只监听 HTTP）。
  桌面端接入服务端时也应只接受 HTTPS，除非两端都在可信内网。
- 注册 token 保存在 `devices.json` 中，文件权限由运维保证；生产环境应放在仅
  `molink-server` 用户可读的目录。
- 开放注册只做了每 IP 每小时 20 次的限制；公网部署应设置 `--registration-token`，
  并在反向代理上再做限流。

## 下一步

- 桌面核心自动注册 / 心跳，并在设置页显示服务端设备 ID。
- 前端“通过设备 ID 连接”：查询地址后直连（验证码仍由对方展示）。
- 中继 / TURN：直连失败时经服务端转发，媒体仍走现有 MOLK 帧协议。
- 移动端 Web 客户端与用户系统。
