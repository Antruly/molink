import {
  PacketType,
  ProtocolError,
  decodePacket,
  parseFramePayload,
  type Frame,
} from './protocol'

export type ClientPhase = 'idle' | 'connecting' | 'live' | 'error'

export interface Welcome {
  session: string
  session_id: number
  protocol: number
  service: string
  version: string
  transport: string
  tiles: {
    codec: string
    screen_w: number
    screen_h: number
    tile_w: number
    tile_h: number
    synthetic: boolean
  }
}

export interface ClientHandlers {
  onPhase: (phase: ClientPhase, detail?: string) => void
  onWelcome: (welcome: Welcome) => void
  onFrame: (frame: Frame, byteLength: number) => void
  onLatency: (ms: number) => void
  onError: (message: string) => void
}

interface ControlMessage {
  type?: string
  code?: string
  message?: string
  session?: string
}

export class MolinkClient {
  private ws: WebSocket | null = null
  private handlers: Partial<ClientHandlers> = {}
  private pendingFrame = false
  private pingTimer: number | null = null
  private closedByUser = false

  setHandlers(handlers: Partial<ClientHandlers>): void {
    this.handlers = handlers
  }

  connect(host: string, port: number, token: string): void {
    this.disconnect()
    this.closedByUser = false

    const url = `ws://${host}:${port}/ws/session`
    this.handlers.onPhase?.('connecting')

    let ws: WebSocket
    try {
      ws = new WebSocket(url)
    } catch {
      this.handlers.onPhase?.('error', 'invalid endpoint')
      return
    }
    ws.binaryType = 'arraybuffer'
    this.ws = ws

    ws.onopen = () => {
      this.sendJson({ type: 'hello', token, client: 'molink-web', protocol: 1 })
    }

    ws.onmessage = (event: MessageEvent) => {
      if (typeof event.data === 'string') {
        this.handleText(event.data)
      } else if (event.data instanceof ArrayBuffer) {
        this.handleBinary(event.data)
      }
    }

    ws.onerror = () => {
      // The close event carries the useful detail; just report a generic
      // failure here so the UI does not sit in "connecting" forever.
      if (!this.closedByUser) {
        this.handlers.onPhase?.('error', 'socket error')
      }
    }

    ws.onclose = (event: CloseEvent) => {
      this.stopPing()
      this.pendingFrame = false
      this.ws = null
      if (this.closedByUser) {
        this.handlers.onPhase?.('idle')
        return
      }
      const reason = event.reason ? `: ${event.reason}` : ''
      this.handlers.onPhase?.('error', `closed (${event.code})${reason}`)
    }
  }

  disconnect(): void {
    this.closedByUser = true
    this.stopPing()
    const ws = this.ws
    this.ws = null
    if (ws && ws.readyState <= WebSocket.OPEN) {
      ws.close(1000, 'client closing')
    }
  }

  isLive(): boolean {
    return this.ws !== null && this.ws.readyState === WebSocket.OPEN
  }

  /** Asks for one synthetic frame. Returns false while one is in flight. */
  requestFrame(): boolean {
    if (!this.isLive() || this.pendingFrame) return false
    this.pendingFrame = true
    this.sendJson({ type: 'request-frame' })
    return true
  }

  sendInput(payload: Record<string, unknown>): void {
    this.sendJson({ type: 'input', ...payload })
  }

  private handleText(text: string): void {
    let message: ControlMessage
    try {
      message = JSON.parse(text) as ControlMessage
    } catch {
      this.handlers.onError?.('control message was not valid JSON')
      return
    }

    switch (message.type) {
      case 'welcome':
        this.handlers.onWelcome?.(message as unknown as Welcome)
        this.handlers.onPhase?.('live')
        this.startPing()
        break
      case 'hello_required':
        // Expected before our hello is processed; nothing to report.
        break
      case 'pong': {
        const sent = (message as { t?: number }).t
        if (typeof sent === 'number') {
          this.handlers.onLatency?.(Math.max(0, performance.now() - sent))
        }
        break
      }
      case 'ping':
        this.sendJson({ type: 'pong', t: (message as { t?: number }).t })
        break
      case 'error':
        this.handlers.onError?.(message.message ?? message.code ?? 'error')
        break
      default:
        break
    }
  }

  private handleBinary(buffer: ArrayBuffer): void {
    try {
      const packet = decodePacket(buffer)
      if (packet.type !== PacketType.frame) return
      this.pendingFrame = false
      const frame = parseFramePayload(packet.payload)
      this.handlers.onFrame?.(frame, buffer.byteLength)
    } catch (error) {
      this.pendingFrame = false
      const message =
        error instanceof ProtocolError ? error.message : 'could not decode frame'
      this.handlers.onError?.(message)
    }
  }

  private startPing(): void {
    this.stopPing()
    const ping = () => {
      if (!this.isLive()) return
      this.sendJson({ type: 'ping', t: performance.now() })
    }
    ping()
    this.pingTimer = window.setInterval(ping, 3000)
  }

  private stopPing(): void {
    if (this.pingTimer !== null) {
      window.clearInterval(this.pingTimer)
      this.pingTimer = null
    }
  }

  private sendJson(payload: Record<string, unknown>): void {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return
    this.ws.send(JSON.stringify(payload))
  }
}
