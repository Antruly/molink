import {
  MolinkClient,
  type ClientPhase,
  type CursorPosition,
  type CursorShape,
  type Welcome,
} from './client'
import type { Frame } from './protocol'

export interface Stats {
  /** Frames that were actually painted in the last second. */
  fps: number
  /** Completed frame pulls (including empty frames) in the last second. */
  wireFps: number
  latencyMs: number | null
  bytesPerSec: number
  frames: number
}

export interface TabPatch {
  phase?: ClientPhase
  welcome?: Welcome | null
  frame?: Frame | null
  cursorShape?: CursorShape | null
  cursorPosition?: CursorPosition | null
  stats?: Stats
  error?: string | null
  controlEnabled?: boolean
  label?: string
  displayIndex?: number
}

export interface SessionSink {
  onPatch: (patch: TabPatch) => void
  onClipboard: (text: string) => void
}

const emptyStats = (): Stats => ({
  fps: 0,
  wireFps: 0,
  latencyMs: null,
  bytesPerSec: 0,
  frames: 0,
})

/// One tab's WebSocket connection plus its one-second statistics windows.
export class SessionConnection {
  private readonly client = new MolinkClient()
  private frameTimes: number[] = []
  private packetTimes: number[] = []
  private byteWindow: { t: number; n: number }[] = []
  private latency: number | null = null
  private frames = 0
  private lastWirePatch = 0

  constructor(private readonly sink: SessionSink) {
    this.client.setHandlers({
      onPhase: (phase, detail) => {
        if (phase === 'error') {
          this.sink.onPatch({
            phase,
            error: detail ?? 'connection failed',
            frame: null,
          })
          return
        }
        if (phase === 'live') {
          this.sink.onPatch({ phase, error: null })
          return
        }
        this.sink.onPatch({ phase })
      },
      onWelcome: (welcome) => {
        this.sink.onPatch({
          welcome,
          displayIndex: welcome.display_index ?? 0,
          ...(welcome.device_id ? { label: welcome.device_id } : {}),
        })
      },
      onDisplay: (index) => this.sink.onPatch({ displayIndex: index }),
      onWireFrame: () => {
        const now = performance.now()
        this.packetTimes.push(now)
        // Empty frames paint nothing, so without this a static screen would
        // leave the auto controller without a link-rate signal.
        if (now - this.lastWirePatch >= 500) {
          this.lastWirePatch = now
          this.sink.onPatch({ stats: this.snapshot(now) })
        }
      },
      onFrame: (frame, bytes) => {
        const now = performance.now()
        this.frames += 1
        this.frameTimes.push(now)
        this.byteWindow.push({ t: now, n: bytes })
        this.sink.onPatch({ frame, stats: this.snapshot(now) })
      },
      onLatency: (ms) => {
        this.latency = ms
        this.sink.onPatch({ stats: this.snapshot() })
      },
      onError: (message) => this.sink.onPatch({ error: message }),
      onCursor: (cursorShape) => this.sink.onPatch({ cursorShape }),
      onCursorPosition: (cursorPosition) =>
        this.sink.onPatch({ cursorPosition }),
      onClipboard: (text) => this.sink.onClipboard(text),
    })
  }

  private snapshot(now = performance.now()): Stats {
    while (this.frameTimes.length > 0 && now - this.frameTimes[0]! > 1000) {
      this.frameTimes.shift()
    }
    while (this.packetTimes.length > 0 && now - this.packetTimes[0]! > 1000) {
      this.packetTimes.shift()
    }
    while (this.byteWindow.length > 0 && now - this.byteWindow[0]!.t > 1000) {
      this.byteWindow.shift()
    }
    return {
      fps: this.frameTimes.length,
      wireFps: this.packetTimes.length,
      latencyMs: this.latency,
      bytesPerSec: this.byteWindow.reduce((sum, item) => sum + item.n, 0),
      frames: this.frames,
    }
  }

  connect(host: string, port: number, token: string): void {
    this.frameTimes = []
    this.packetTimes = []
    this.byteWindow = []
    this.latency = null
    this.frames = 0
    this.lastWirePatch = 0
    this.sink.onPatch({ stats: emptyStats(), error: null, frame: null })
    this.client.connect(host, port, token)
  }

  disconnect(): void {
    this.client.disconnect()
  }

  requestFrame(): void {
    this.client.requestFrame()
  }

  sendInput(payload: Record<string, unknown>): void {
    this.client.sendInput(payload)
  }

  setCaptureFps(fps: number): void {
    this.client.setCaptureFps(fps)
  }

  setQuality(mode: string, fps?: number): void {
    this.client.setQuality(mode, fps)
  }

  setDisplay(index: number): void {
    this.client.setDisplay(index)
  }

  requestClipboard(): void {
    this.client.requestClipboard()
  }

  setClipboard(text: string): void {
    this.client.setClipboard(text)
  }
}
