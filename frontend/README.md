# moLink 前端

React 19 + TypeScript + Vite + Tailwind 4 + Zustand + react-i18next。通过 WebSocket
连接本机 `molink-core`，把 MOLK 帧包渲染到 Canvas。

## 开发

```bash
pnpm install
pnpm dev        # http://localhost:1420
```

本机页面（peer 为回环地址）会自动通过 `/api/device` 取得本机令牌并进入主页，
无需手动输入验证码。开发时打开：

```
http://localhost:1420/?host=127.0.0.1&port=21118
```

来自其他设备的访问由核心托管同一套页面，打开时会显示验证码登录页；验证通过后
才能进入主页，并可从这台设备连接其他设备或一键控制核心所在主机。

空闲锁定由后台驱动：默认 60 秒无操作且无远程会话时锁定，锁定后需要重新输入验证码。

## 脚本

| 命令 | 说明 |
|---|---|
| `pnpm dev` | Vite 开发服务器，固定 1420 端口 |
| `pnpm build` | `tsc --noEmit` + `vite build` |
| `pnpm typecheck` | 仅类型检查 |
| `pnpm lint` | ESLint |
| `pnpm preview` | 预览 `dist/` |

## 结构

```
src/
├── lib/protocol.ts   MOLK 编解码（与 backend/src/protocol/frame_packet.h 对应）
├── lib/client.ts     WebSocket 客户端：hello、心跳、请求帧、延迟测量
├── lib/api.ts        /api/state、登录、活动上报、锁定设置
├── lib/session-connection.ts  单个会话的连接与统计
├── store/app.ts      认证状态机、多会话 Tab、主题与偏好
├── components/       AuthGate、设备页、会话页、标签栏、自绘标题栏
├── locales/          en / zh-CN
└── i18n.ts
```

协议细节见 [../docs/protocol.md](../docs/protocol.md)。

## 已知限制

- 只渲染 `bgra` 与 `deflate_bgra` 瓦片；H.264/AV1 的 WebCodecs 路径尚未接入。
- `deflate_bgra` 依赖浏览器的 `DecompressionStream("deflate")`（Chromium 80+、
  WebKit 16.4+）。
- 控制模式需要在会话页手动开启，默认不接管输入。
- 锁定状态由后台判定；前端每 3 秒读取 `/api/state`，并在用户操作时上报活动。
- 远端光标由后台单独发送，控制模式开启时隐藏覆盖层（由本地光标指示）。
- 剪贴板按钮依赖浏览器 Clipboard API；非安全上下文或未授权时会提示错误。
- 帧率与带宽取决于后台抓屏与 zlib 压缩，未使用视频编码器。
