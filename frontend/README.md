# moLink 前端

React 19 + TypeScript + Vite + Tailwind 4 + Zustand + react-i18next。通过 WebSocket
连接本机 `molink-core`，把 MOLK 帧包渲染到 Canvas。

## 开发

```bash
pnpm install
pnpm dev        # http://localhost:1420
```

登录页填入后台启动时打印的令牌。也可以直接带参数自动连接：

```
http://localhost:1420/?host=127.0.0.1&port=21118&token=<token>
```

Tauri 壳层就是用这种 URL 注入端口与令牌的。

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
├── store/session.ts  Zustand 状态与统计窗口
├── components/       连接页、会话页、Canvas 渲染
├── locales/          en / zh-CN
└── i18n.ts
```

协议细节见 [../docs/protocol.md](../docs/protocol.md)。

## 已知限制

- 只渲染 `bgra` 与 `deflate_bgra` 瓦片；H.264/AV1 的 WebCodecs 路径尚未接入。
- `deflate_bgra` 依赖浏览器的 `DecompressionStream("deflate")`（Chromium 80+、
  WebKit 16.4+）。
- 输入注入依赖后台的 XTEST 支持；控制模式需要在会话页手动开启，默认不接管输入。
- 帧率与带宽取决于后台抓屏与 zlib 压缩，未使用视频编码器。
