# moLink Server 分布式与 Pro 管理（第三阶段设计）

> 状态：设计稿，尚未实现。开源版 `molink-server` 始终是**独立可运行的单节点**；
> 分布式控制面属于商业版（Pro）仓库，在开源版之上通过稳定的扩展点接入。

## 1. 目标与非目标

目标：

- 开源单节点保持零外部依赖即可自托管；不引入控制面也能完整工作。
- Pro 控制面统一管理多个开源节点：下发配置、收集状态、管理设备和路由。
- 控制面支持主备/高可用，节点与控制面之间只建立**出站**连接。
- Pro 仓库可以持续从开源仓库同步代码，而不是分叉维护。

非目标：

- 开源版不内置“节点必须连控制面”的强制逻辑。
- v1 不做媒体中继；分布式只解决**寻址、配置与观测**。

## 2. 节点身份

```json
{
  "cluster_id": "uuid",          // 仅 Pro；开源节点为空
  "node_uuid": "uuid",           // 首次启动生成并持久化
  "node_id": 1,                  // 默认 1；Pro 注册后由控制面分配 >= 1
  "name": "shanghai-1",
  "public_url": "https://..."
}
```

- `node_uuid` 持久化在数据目录的 `node.json`，用于重装/换 IP 后识别同一节点。
- `node_id` 是控制面内的数字标识；未接入时固定为 1，保持开源行为不变。
- 节点身份不参与设备 ID 的生成；设备 ID 仍由各节点独立分配。

## 3. 接入与信任

1. 管理员在控制面生成一次性 join token。
2. 节点配置 `control_plane.url` + `join_token`，启动时向控制面发起 outbound
   `POST /pro/v1/enroll`。
3. 控制面返回节点证书（由集群 CA 签发）与 `node_id`；节点保存为
   `/var/lib/molink-server/pro/certs/`。
4. 之后所有控制通道使用 mTLS；证书到期前自动轮换，控制面可吊销。
5. 控制面永远不主动连入节点；节点断线后按指数退避重连。

## 4. 配置下发

- 配置文档带单调递增的 `config_revision` 和 `sha256`。
- 控制面保存 desired state；节点回报 `applied_revision` 与最近一次错误。
- 节点先校验（schema、CIDR、限速、TLS 路径），再应用：
  - 热字段：IP 策略、限速、封禁、公开地址、注册令牌。
  - 需重启字段：监听地址/端口、数据库、TLS 证书。
- 应用前保存 last-known-good；新配置启动失败自动回滚并上报。
- 变更审计在控制面与节点两侧都记录；节点只接受来自已认证控制面的配置。
- 开源版预留 `settings` 的版本号与 `PUT` 校验路径，Pro 通过同一接口下发。

## 5. 状态上报

节点每 30 秒（可配）上报批次：

```json
{
  "node_id": 7,
  "node_uuid": "...",
  "version": "0.2.0",
  "applied_revision": 42,
  "devices": {"total": 120, "online": 87},
  "requests_per_minute": 240,
  "active_bans": 3,
  "database": {"driver": "postgres", "schema": 3},
  "health": "ok"
}
```

- 不包含设备令牌、密码、会话或原始客户端 IP 列表。
- 离线时在本地有界队列中缓存，恢复后按时间顺序补报；超过上限丢弃最旧批次。
- 控制面用这些数据做健康检查、容量视图与告警，不直接修改节点数据。

## 6. 高可用与负载均衡

- 控制面使用 PostgreSQL 作为共享状态；主备通过 advisory lock 或 etcd lease 选主。
- 只有 leader 接受写请求；follower 可读并转发写。客户端/节点通过 L4 负载均衡
  访问控制面，leader 切换时节点重连。
- 配置 revision 带 fencing：旧 leader 的写入因 revision 落后被拒绝。
- 目录查询：控制面可选维护全局 `device_id -> node` 映射；客户端查询时返回节点
  的 `public_url`。设备注册仍落在所属节点，避免跨节点写冲突。
- 会话仍由设备所在节点管理；控制面只保存路由元数据，不代理媒体。

## 7. 代码同步与仓库结构

- 开源仓库的 `server/` 是唯一事实来源；Pro 仓库只包含 `pro/` 与部署清单。
- Pro 通过 git subtree/submodule 或定期 merge `server-v*` 标签同步开源代码；
  CI 校验 `server/` 下的文件哈希未被 Pro 修改。
- 开源版提供的扩展点（第三阶段实现）：
  - 配置的 revision/校验/回滚接口；
  - 状态快照（设备数、在线数、限速命中、封禁数）；
  - 可插拔的 IP 策略与限速后端（Pro 可下发）。
- Pro 专属：控制面 API、集群管理 UI、节点证书、分布式配置存储、告警。

## 8. 控制面 API 草案

```
POST /pro/v1/enroll                 一次性 join token -> 节点证书 + node_id
POST /pro/v1/nodes/:id/heartbeat    状态/版本/容量/健康
GET  /pro/v1/nodes/:id/config       当前 desired config + revision
POST /pro/v1/nodes/:id/config/ack   应用结果、错误、applied_revision
POST /pro/v1/telemetry              批量上报（幂等，带节点签名）
GET  /pro/v1/devices/:id            全局设备路由（可选）
```

## 9. 迁移与回退

- 开源节点 -> Pro：只需配置控制面地址与 join token；失败时删除配置即可回到
  独立模式，数据与设备 ID 不变。
- Pro 节点 -> 开源：停止控制面连接，保留最后一次成功配置；开源版忽略 Pro 字段。
- 控制面故障不阻断节点自身的注册/心跳/管理页；配置保持 last-known-good。
