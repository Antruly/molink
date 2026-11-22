import { virtualClipboardBridgeUrl } from './api'

/// Minimal view of SessionConnection: the bridge only needs to send one
/// clipboard-pull request over the remote session.
export interface VirtualPullSender {
  sendFileAction(action: string, extra?: Record<string, unknown>): void
}

interface PendingPull {
  id: number
  offset: number
  seqs: number[]
  timer: number
}

const REPLY_TIMEOUT_MS = 20_000

/// Relays the local core's virtual-file reads to the remote host.
///
/// The core mounts the remote clipboard files and asks for one byte range at a
/// time over `ws://<local core>/ws/clipboard-virtual`. This class forwards
/// each ask as a `clipboard-pull` on the existing session socket and hands the
/// 0x18 reply back to the core as a binary bridge frame. The host protocol is
/// unchanged; only the direction of initiation differs.
export class VirtualClipboardBridge {
  private socket: WebSocket | null = null
  private readonly pending = new Map<string, PendingPull>()
  private disposed = false

  constructor(
    private readonly sender: () => VirtualPullSender | null,
    private readonly onClosed: (reason: string) => void,
  ) {}

  /// Opens the relay socket. Resolves once it is open, rejects on an error or
  /// a stale mount (the core closes with 1008 when the generation is gone).
  open(mount: string): Promise<void> {
    this.close()
    this.disposed = false
    return new Promise((resolve, reject) => {
      let settled = false
      let socket: WebSocket
      try {
        socket = new WebSocket(virtualClipboardBridgeUrl(mount))
      } catch {
        reject(new Error('clipboard_virtual_unavailable'))
        return
      }
      socket.binaryType = 'arraybuffer'
      this.socket = socket
      const timer = window.setTimeout(() => {
        if (settled) return
        settled = true
        reject(new Error('clipboard_virtual_timeout'))
        this.close()
      }, 5000)
      socket.onopen = () => {
        if (settled) return
        settled = true
        window.clearTimeout(timer)
        resolve()
      }
      socket.onerror = () => {
        if (!settled) {
          settled = true
          window.clearTimeout(timer)
          reject(new Error('clipboard_virtual_unavailable'))
        }
      }
      socket.onmessage = (event) => {
        if (typeof event.data === 'string') {
          this.handleRequest(event.data)
        }
      }
      socket.onclose = () => {
        window.clearTimeout(timer)
        if (this.socket === socket) this.socket = null
        this.failPending()
        if (!settled) {
          settled = true
          reject(new Error('clipboard_virtual_unavailable'))
          return
        }
        if (!this.disposed) this.onClosed('bridge closed')
      }
    })
  }

  get active(): boolean {
    return this.socket !== null
  }

  /// Deliberate close: do not report it as the session ending.
  close(): void {
    this.disposed = true
    this.failPending()
    const socket = this.socket
    this.socket = null
    if (socket) {
      socket.onclose = null
      socket.onerror = null
      socket.onmessage = null
      try {
        socket.close()
      } catch {
        // Already closed.
      }
    }
  }

  /// One 0x18 reply from the host. Returns true when a virtual read was
  /// waiting for it, so the download path must not also consume the packet.
  handleChunk(
    id: number,
    status: number,
    offset: number,
    data: Uint8Array,
  ): boolean {
    const key = `${id}:${offset}`
    const pending = this.pending.get(key)
    if (pending === undefined) return false
    window.clearTimeout(pending.timer)
    this.pending.delete(key)
    for (const seq of pending.seqs) {
      this.sendReply(seq, status, offset, data)
    }
    return true
  }

  private handleRequest(raw: string): void {
    let message: {
      type?: unknown
      seq?: unknown
      id?: unknown
      offset?: unknown
      length?: unknown
    }
    try {
      message = JSON.parse(raw) as typeof message
    } catch {
      return
    }
    if (
      message.type !== 'pull' ||
      typeof message.seq !== 'number' ||
      typeof message.id !== 'number' ||
      typeof message.offset !== 'number' ||
      typeof message.length !== 'number' ||
      !Number.isSafeInteger(message.seq) ||
      !Number.isSafeInteger(message.id) ||
      !Number.isSafeInteger(message.offset) ||
      !Number.isSafeInteger(message.length) ||
      message.length <= 0
    ) {
      return
    }
    const connection = this.sender()
    if (connection === null) {
      this.close()
      return
    }
    const key = `${message.id}:${message.offset}`
    const existing = this.pending.get(key)
    if (existing !== undefined) {
      // The same FUSE range was read twice before the first reply arrived.
      // One host pull serves every waiter.
      existing.seqs.push(message.seq)
      return
    }
    const pending: PendingPull = {
      id: message.id,
      offset: message.offset,
      seqs: [message.seq],
      timer: window.setTimeout(() => {
        if (this.pending.get(key) === pending) this.pending.delete(key)
      }, REPLY_TIMEOUT_MS),
    }
    this.pending.set(key, pending)
    connection.sendFileAction('clipboard-pull', {
      id: message.id,
      offset: message.offset,
      length: message.length,
    })
  }

  private sendReply(
    seq: number,
    status: number,
    offset: number,
    data: Uint8Array,
  ): void {
    const socket = this.socket
    if (socket === null || socket.readyState !== WebSocket.OPEN) return
    const frame = new Uint8Array(16 + data.byteLength)
    const view = new DataView(frame.buffer)
    view.setUint32(0, seq >>> 0, true)
    view.setUint32(4, status >>> 0, true)
    view.setBigUint64(8, BigInt(offset), true)
    frame.set(data, 16)
    try {
      socket.send(frame.buffer)
    } catch {
      this.close()
    }
  }

  private failPending(): void {
    for (const pending of this.pending.values()) {
      window.clearTimeout(pending.timer)
    }
    this.pending.clear()
  }
}
