import {
  MolinkClient,
  type ClientPhase,
  type CursorPosition,
  type CursorShape,
  type SessionEvent,
  type SessionEventListener,
  type Welcome,
} from './client'
import type { Frame } from './protocol'
import { ClipboardFileSync } from './clipboard-sync'

export interface Stats {
  /** Frames that were actually painted in the last second. */
  fps: number
  /** Completed frame pulls (including empty frames) in the last second. */
  wireFps: number
  latencyMs: number | null
  bytesPerSec: number
  frames: number
  /** Frames dropped as stale or overtaken in the last second. */
  dropped: number
}

export interface TabPatch {
  phase?: ClientPhase
  welcome?: Welcome | null
  frame?: Frame | null
  cursorShape?: CursorShape | null
  cursorPosition?: CursorPosition | null
  stats?: Stats
  error?: string | null
  errorCode?: string | null
  transport?: string
  notice?: string | null
  approvalDeadline?: number | null
  retryAt?: number | null
  controlEnabled?: boolean
  clipboardEnabled?: boolean
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
  dropped: 0,
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
  private clipboardTimer: number | null = null
  private lastClipboard = ''
  private clipboardFailures = 0
  private dropTimes: number[] = []
  private presetFps = 0
  private congested = false
  private lastDropAt = 0
  private congestionTimer: number | null = null
  private clipboardEnabled = true
  /// This viewer's machine name/ID, sent in hello so the shared chat can
  /// attribute its messages. Empty for a plain browser.
  private clientName = ''
  private clientId = ''
  /// Sticky across manual reconnects (Windows 7 forces TCP, and the user may
  /// have checked "force relay").
  private forceRelay = false
  /// The active tab keeps the full-rate pull loop in SessionView; every other
  /// live tab pulls slowly here so it stays current without painting.
  private foreground = true
  private backgroundTimer: number | null = null
  /// Files copied on this machine are pushed to every remote session, in the
  /// background too. Only the file panel's own uploads pause it.
  private fileSync: ClipboardFileSync | null = null
  private fileSyncTimer: number | null = null
  private fileSyncPaused = false
  private readonly listeners = new Set<SessionEventListener>()

  constructor(private readonly sink: SessionSink) {
    this.client.setHandlers({
      onPhase: (phase, detail) => {
        if (phase === 'error') {
          this.stopBackgroundPull()
          this.stopFileSync()
          this.sink.onPatch({
            phase,
            error: detail ?? 'connection failed',
            notice: null,
            approvalDeadline: null,
            frame: null,
          })
          return
        }
        if (phase === 'disconnected') {
          this.stopBackgroundPull()
          this.stopFileSync()
          this.sink.onPatch({
            phase,
            error: null,
            errorCode: null,
            notice: 'connection_lost',
          })
          return
        }
        if (phase === 'live') {
          this.sink.onPatch({
            phase,
            error: null,
            errorCode: null,
            notice: null,
            approvalDeadline: null,
            retryAt: null,
          })
          this.startClipboardSync()
          this.startCongestionWatch()
          if (!this.foreground) this.startBackgroundPull()
          this.startFileSync()
          return
        }
        this.stopBackgroundPull()
        this.sink.onPatch({
          phase,
          ...(phase === 'connecting'
            ? { error: null, errorCode: null, notice: null }
            : {}),
        })
      },
      onAccessRequested: () => {
        this.sink.onPatch({
          phase: 'connecting',
          error: null,
          errorCode: null,
          notice: 'waiting_approval',
          approvalDeadline: Date.now() + 60_000,
        })
      },
      onWelcome: (welcome) => {
        // The computer name is the user-facing identity. The device ID is a
        // phase-2 server registration concept and is only a fallback.
        const label = welcome.device_name || welcome.device_id
        this.sink.onPatch({
          welcome,
          displayIndex: welcome.display_index ?? 0,
          ...(label ? { label } : {}),
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
      onError: (message, code) =>
        this.sink.onPatch({ error: message, errorCode: code ?? null, notice: null }),
      onTransport: (mode) => this.sink.onPatch({ transport: mode }),
      onCursor: (cursorShape) => this.sink.onPatch({ cursorShape }),
      onCursorPosition: (cursorPosition) =>
        this.sink.onPatch({ cursorPosition }),
      onRetryScheduled: (atMs) => {
        this.sink.onPatch({
          phase: 'disconnected',
          notice: 'connection_lost',
          retryAt: atMs,
        })
      },
      onFrameDropped: () => {
        const now = performance.now()
        this.dropTimes.push(now)
        this.lastDropAt = now
        if (!this.congested) {
          // Backlog: cut the host capture rate so new frames do not queue up
          // behind the ones we are already dropping.
          this.congested = true
          this.client.setCaptureFps(8)
        }
      },
      onClipboard: (text) => {
        if (!this.clipboardEnabled) return
        // Only the remote the user is actively working in may write the local
        // clipboard; otherwise several tabs fight over it. Local -> remote
        // still broadcasts from every session below.
        if (!this.foreground) return
        if (typeof document !== 'undefined' && !document.hasFocus()) return
        // Remember what we just wrote so the poll does not echo it back.
        this.lastClipboard = text
        this.sink.onClipboard(text)
      },
      onSessionEvent: (event: SessionEvent) => {
        if (event.kind === 'file') this.fileSync?.handleMessage(event.message)
        for (const listener of this.listeners) listener(event)
      },
    })
  }

  /// Chat, files and other session-scoped events. Returns an unsubscribe.
  addListener(listener: SessionEventListener): () => void {
    this.listeners.add(listener)
    return () => this.listeners.delete(listener)
  }

  /// Text clipboard sync is a privacy-sensitive feature; the floating toolbar
  /// exposes it as a per-session switch.
  setClipboardEnabled(enabled: boolean): void {
    this.clipboardEnabled = enabled
    if (!enabled) {
      this.stopClipboardSync()
      return
    }
    this.startClipboardSync()
  }

  /// Enable local file clipboard -> this remote. Called for every session
  /// when the UI runs on the local core; the token provider is evaluated per
  /// poll because the local session token may arrive after boot.
  enableClipboardFileSync(token: () => string): void {
    if (this.fileSync !== null) return
    this.fileSync = new ClipboardFileSync(
      () => this,
      token,
      (message) => this.sink.onPatch({ error: message, errorCode: null }),
    )
  }

  /// The files panel uploads through the same ids; pause the automatic
  /// clipboard sync while it is open so the two cannot race.
  setFilePanelOpen(open: boolean): void {
    this.fileSyncPaused = open
  }

  /// Files dropped onto the remote picture: upload them, put them on the
  /// remote clipboard and paste them into the focused remote window.
  uploadDroppedFiles(files: File[], pasteAfterUpload: boolean): void {
    if (files.length === 0) return
    if (this.fileSync === null) {
      // A remote-browser session has no local clipboard API; the drop still
      // works, so create the transfer helper without a token.
      this.fileSync = new ClipboardFileSync(
        () => this,
        () => '',
        (message) => this.sink.onPatch({ error: message, errorCode: null }),
      )
    }
    void this.fileSync.uploadFiles(files, pasteAfterUpload)
  }

  private startFileSync(): void {
    if (this.fileSync === null || this.fileSyncTimer !== null) return
    this.fileSyncTimer = window.setInterval(() => {
      if (this.fileSyncPaused || !this.client.isLive()) return
      void this.fileSync?.poll()
    }, 1200)
  }

  private stopFileSync(): void {
    if (this.fileSyncTimer !== null) {
      window.clearInterval(this.fileSyncTimer)
      this.fileSyncTimer = null
    }
    this.fileSync?.reset()
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
    while (this.dropTimes.length > 0 && now - this.dropTimes[0]! > 1000) {
      this.dropTimes.shift()
    }
    return {
      fps: this.frameTimes.length,
      wireFps: this.packetTimes.length,
      latencyMs: this.latency,
      bytesPerSec: this.byteWindow.reduce((sum, item) => sum + item.n, 0),
      frames: this.frames,
      dropped: this.dropTimes.length,
    }
  }

  connect(host: string, port: number, token: string,
          requestAccess = false, deviceId = '', forceRelay?: boolean,
          clientName?: string, clientId?: string): void {
    // Keep the identity and TCP-only choice across manual reconnects; only the
    // first connect supplies them.
    if (forceRelay !== undefined) this.forceRelay = forceRelay
    if (clientName !== undefined) this.clientName = clientName
    if (clientId !== undefined) this.clientId = clientId
    this.stopBackgroundPull()
    this.stopFileSync()
    this.stopClipboardSync()
    this.stopCongestionWatch()
    this.lastClipboard = ''
    this.clipboardFailures = 0
    this.dropTimes = []
    this.congested = false
    this.lastDropAt = 0
    this.frameTimes = []
    this.packetTimes = []
    this.byteWindow = []
    this.latency = null
    this.frames = 0
    this.lastWirePatch = 0
    this.sink.onPatch({
      stats: emptyStats(),
      error: null,
      notice: null,
      frame: null,
    })
    this.client.connect(host, port, token, requestAccess, deviceId,
                        this.forceRelay, this.clientName, this.clientId)
  }

  disconnect(): void {
    this.stopBackgroundPull()
    this.stopFileSync()
    this.stopClipboardSync()
    this.stopCongestionWatch()
    this.client.disconnect()
  }

  /// Called when this tab becomes (or stops being) the visible one. The
  /// inactive tabs keep pulling at 1 fps so their frame in the store stays
  /// current and the session never goes idle; only the foreground view pulls
  /// at full rate.
  setForeground(active: boolean): void {
    if (this.foreground === active) return
    this.foreground = active
    if (active) {
      this.stopBackgroundPull()
      // The full-rate pull loop is owned by SessionView; ask for a fresh
      // keyframe so the switch shows a complete picture immediately.
      this.client.requestKeyframe()
      this.client.requestFrame()
      // Focus also pulls the remote clipboard once, so a text copied while
      // this tab was in the background becomes available locally.
      this.client.requestClipboard()
    } else if (this.client.isLive()) {
      this.startBackgroundPull()
    }
  }

  private startBackgroundPull(): void {
    if (this.backgroundTimer !== null) return
    this.backgroundTimer = window.setInterval(() => {
      if (!this.client.isLive()) return
      this.client.requestFrame()
    }, 1000)
  }

  private stopBackgroundPull(): void {
    if (this.backgroundTimer === null) return
    window.clearInterval(this.backgroundTimer)
    this.backgroundTimer = null
  }

  reconnectNow(): void {
    this.client.reconnectNow()
  }

  private startCongestionWatch(): void {
    this.stopCongestionWatch()
    this.congestionTimer = window.setInterval(() => {
      if (!this.congested) return
      if (performance.now() - this.lastDropAt <= 5000) return
      this.congested = false
      this.client.setCaptureFps(this.presetFps > 0 ? this.presetFps : 15)
    }, 1000)
  }

  private stopCongestionWatch(): void {
    if (this.congestionTimer !== null) {
      window.clearInterval(this.congestionTimer)
      this.congestionTimer = null
    }
  }

  private startClipboardSync(): void {
    this.stopClipboardSync()
    if (!this.clipboardEnabled) return
    const clipboard = navigator.clipboard
    if (!clipboard?.readText) return
    const poll = () => {
      clipboard.readText().then((text) => {
        this.clipboardFailures = 0
        if (text !== '' && text !== this.lastClipboard) {
          this.lastClipboard = text
          this.client.sendClipboardSync(text)
        }
      }).catch(() => {
        this.clipboardFailures += 1
        // Browsers require a gesture/permission for clipboard reads. Retry on
        // the next interaction instead of spamming the console.
        if (this.clipboardFailures >= 3) this.stopClipboardSync()
      })
    }
    poll()
    this.clipboardTimer = window.setInterval(poll, 1000)
  }

  private stopClipboardSync(): void {
    if (this.clipboardTimer !== null) {
      window.clearInterval(this.clipboardTimer)
      this.clipboardTimer = null
    }
  }

  requestFrame(): void {
    this.client.requestFrame()
  }

  requestKeyframe(): void {
    this.client.requestKeyframe()
  }

  sendInput(payload: Record<string, unknown>): void {
    this.client.sendInput(payload)
  }

  setCaptureFps(fps: number): void {
    this.presetFps = fps
    this.client.setCaptureFps(this.congested ? Math.min(fps, 8) : fps)
  }

  setQuality(mode: string, fps?: number): void {
    this.client.setQuality(mode, fps)
  }

  setDisplay(index: number): void {
    this.client.setDisplay(index)
  }

  setDisplayMode(width: number, height: number): void {
    this.client.setDisplayMode(width, height)
  }

  /// Clears an application-level error shown in the tab (for example a
  /// rejected display-mode change) without touching the connection.
  dismissError(): void {
    this.sink.onPatch({ error: null, errorCode: null })
  }

  requestClipboard(): void {
    this.client.requestClipboard()
  }

  requestClipboardFiles(): void {
    this.client.requestClipboardFiles()
  }

  startAudio(): void {
    this.client.startAudio()
  }

  stopAudio(): void {
    this.client.stopAudio()
  }

  inviteCall(): void {
    this.client.inviteCall()
  }

  hangupCall(): void {
    this.client.hangupCall()
  }

  sendVoice(pcm: Uint8Array): void {
    this.client.sendVoice(pcm)
  }

  setClipboard(text: string): void {
    this.client.setClipboard(text)
  }

  sendChat(text: string): void {
    this.client.sendChat(text)
  }

  requestChatHistory(): void {
    this.client.requestChatHistory()
  }

  sendFileAction(
    action: string,
    extra: Record<string, unknown> = {},
  ): void {
    this.client.sendFileAction(action, extra)
  }

  sendFileChunk(id: number, offset: number, data: Uint8Array): void {
    this.client.sendFileChunk(id, offset, data)
  }
}
