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
import { PcmPlayer } from './audio'
import { ClipboardFileSync } from './clipboard-sync'
import {
  applyFrameToSession,
  applyJpegFrameToSession,
  forgetSessionScreenBuffer,
} from './screen-buffer'

export interface Stats {
  /** Frames that were actually painted in the last second. */
  fps: number
  /** Completed frame pulls (including empty frames) in the last second. */
  wireFps: number
  latencyMs: number | null
  /**
   * Bytes per second the session actually costs on the network. An AV1 patch
   * is decoded by the client core before it reaches the page, so the bytes the
   * page receives are several times the wire bytes; while the core reports a
   * wire rate, that rate is used instead of the received-byte count.
   */
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
  /// The client core decoded an AV1 patch within the last few seconds.
  av1Active?: boolean
  /// True when the wire carries AV1 only, not AV1 patches over a tile image.
  av1Full?: boolean
  av1WireBytes?: number
  av1DecodedBytes?: number
  /// Host-reported capture rate and encoder build time (EMA).
  hostFps?: number
  hostBuildMs?: number
  /// Preset another viewer selected on the shared host.
  sharedQuality?: { mode: string }
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
  private clipboardRetryAt = 0
  private dropTimes: number[] = []
  private av1Timer: number | null = null
  /// Wire rate reported by the client core with its codec stat, and when it
  /// arrived. Only the core sees the host's packets before transcoding.
  private wireRate: number | null = null
  private wireRateAt = 0
  /// Audio playback lives with the connection so a background tab keeps
  /// playing; created from the format the welcome announced.
  private player: PcmPlayer | null = null
  private audioVolume = 0.8
  private audioFormat: { sampleRate: number; channels: number } | null = null
  /// Host accepts per-second viewer health reports (welcome.viewer_stats).
  private viewerStats = false
  private viewerTimer: number | null = null
  private clipboardEnabled = true
  /// This viewer's machine name/ID, sent in hello so the shared chat can
  /// attribute its messages. Empty for a plain browser.
  private clientName = ''
  private clientId = ''
  /// Sticky across manual reconnects (Windows 7 forces TCP, and the user may
  /// have checked "force relay").
  private forceRelay = false
  /// The active tab keeps the full-rate pull loop in SessionView; every other
  /// live tab pulls slowly here. Frames still land in that session's composited
  /// buffer, so switching back is instant instead of a black canvas.
  private foreground = true
  private backgroundTimer: number | null = null
  /// Files copied on this machine are pushed to every remote session, in the
  /// background too. Only the file panel's own uploads pause it.
  private fileSync: ClipboardFileSync | null = null
  private fileSyncTimer: number | null = null
  private fileSyncPaused = false
  private readonly listeners = new Set<SessionEventListener>()

  constructor(
    private readonly sink: SessionSink,
    /// Tab id owning the composited back buffer. It outlives this component,
    /// so a tab switch can paint immediately.
    private readonly sessionKey = '',
  ) {
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
        const format = welcome.audio_format
        this.audioFormat =
          format !== undefined && format !== null
            ? {
                sampleRate: format.sample_rate || 48000,
                channels: format.channels || 2,
              }
            : null
        this.viewerStats = welcome.viewer_stats === true
        this.startViewerStats()
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
      onFrame: (frame, bytes, bitmaps) => {
        // Compose into the session buffer even while the tab is in the
        // background: the pull loop keeps every live tab current, and a
        // switch then shows the last picture instead of an empty canvas.
        if (bitmaps !== undefined && bitmaps.length > 0) {
          applyJpegFrameToSession(this.sessionKey, frame, bitmaps)
        } else {
          applyFrameToSession(this.sessionKey, frame)
        }
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
      onTransport: (mode, path, protocol) =>
        this.sink.onPatch({
          transport: mode,
          ...(path ? { path } : {}),
          ...(protocol ? { protocol } : {}),
        }),
      onCodec: (info) => {
        // Patches arrive continuously while a lossy preset is in use; the
        // badge disappears if none have arrived for five seconds.
        if (this.av1Timer !== null) window.clearTimeout(this.av1Timer)
        if (info.wireRate > 0) {
          this.wireRate = info.wireRate
          this.wireRateAt = performance.now()
        }
        this.sink.onPatch({
          av1Active: true,
          av1Full: info.fullFrame,
          av1WireBytes: info.wireBytes,
          av1DecodedBytes: info.decodedBytes,
        })
        this.av1Timer = window.setTimeout(() => {
          this.av1Timer = null
          this.sink.onPatch({ av1Active: false })
        }, 10000)
      },
      onQualityShared: (mode) =>
        this.sink.onPatch({ sharedQuality: { mode } }),
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
        // Recorded as a congestion signal for the frame-rate controller in
        // SessionView, which owns the single host-wide rate decision.
        this.dropTimes.push(performance.now())
      },
      onCaptureStats: (info) =>
        this.sink.onPatch({
          hostFps: info.fps,
          hostBuildMs: info.buildMs,
        }),
      onClipboard: (text) => {
        if (!this.clipboardEnabled) return
        // Only the remote the user is actively working in may write the local
        // clipboard; otherwise several tabs fight over it. Local -> remote
        // still broadcasts from every session below.
        if (!this.foreground) return
        // Do not require page focus: the whole point is to paste on this
        // machine after copying on the remote, which usually happens after
        // switching away. The write itself is still guarded by the sink.
        // Remember what we just wrote so the poll does not echo it back.
        this.lastClipboard = text
        this.sink.onClipboard(text)
      },
      onSessionEvent: (event: SessionEvent) => {
        if (event.kind === 'file') this.fileSync?.handleMessage(event.message)
        if (event.kind === 'audio') this.player?.push(event.data)
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

  /// Once a second, tell the host how this viewer is actually doing. The host
  /// halves large pictures when the client cannot consume full detail, which
  /// is what stops the 5 fps / 13 MB/s oscillation on a slow host.
  private startViewerStats(): void {
    if (!this.viewerStats || this.viewerTimer !== null) return
    this.viewerTimer = window.setInterval(() => {
      const stats = this.snapshot()
      this.client.sendViewerStats(stats.fps, stats.dropped, stats.bytesPerSec)
    }, 1000)
  }

  private stopViewerStats(): void {
    if (this.viewerTimer === null) return
    window.clearInterval(this.viewerTimer)
    this.viewerTimer = null
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
    // A codec stat is current for a few seconds: a static screen stops
    // producing patches, and the last wire rate is then still the best
    // description of what the session costs.
    const currentWireRate =
      this.wireRate !== null && now - this.wireRateAt < 4000
        ? this.wireRate
        : null
    return {
      fps: this.frameTimes.length,
      wireFps: this.packetTimes.length,
      latencyMs: this.latency,
      bytesPerSec:
        currentWireRate ??
        this.byteWindow.reduce((sum, item) => sum + item.n, 0),
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
    this.lastClipboard = ''
    this.clipboardFailures = 0
    this.clipboardRetryAt = 0
    this.dropTimes = []
    this.frameTimes = []
    this.packetTimes = []
    this.byteWindow = []
    this.latency = null
    this.wireRate = null
    this.wireRateAt = 0
    this.stopViewerStats()
    this.stopAv1Indicator()
    forgetSessionScreenBuffer(this.sessionKey)
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

  private stopAv1Indicator(): void {
    if (this.av1Timer !== null) {
      window.clearTimeout(this.av1Timer)
      this.av1Timer = null
    }
    this.sink.onPatch({ av1Active: false })
  }

  disconnect(): void {
    this.stopAudio()
    this.stopViewerStats()
    this.stopAv1Indicator()
    this.stopBackgroundPull()
    this.stopFileSync()
    this.stopClipboardSync()
    this.client.disconnect()
  }

  /// Called when this tab becomes (or stops being) the visible one. The
  /// inactive tabs keep pulling at 1 fps so their back buffer and stored frame
  /// stay current and the session never goes idle; only the foreground view
  /// pulls at full rate.
  setForeground(active: boolean): void {
    if (this.foreground === active) return
    this.foreground = active
    if (active) {
      this.stopBackgroundPull()
      // The full-rate pull loop is owned by SessionView. The session buffer
      // already holds the last picture, so a fresh frame is enough: forcing a
      // keyframe here made every tab switch re-encode the whole screen (and a
      // full-screen AV1 keyframe is expensive on the host).
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

  private startClipboardSync(): void {
    this.stopClipboardSync()
    if (!this.clipboardEnabled) return
    const clipboard = navigator.clipboard
    if (!clipboard?.readText) return
    const poll = () => {
      // Browsers require focus (and sometimes a gesture) for clipboard reads.
      // A hidden window would only produce permission errors.
      if (document.visibilityState === 'hidden') return
      if (Date.now() < this.clipboardRetryAt) return
      clipboard.readText().then((text) => {
        this.clipboardFailures = 0
        this.clipboardRetryAt = 0
        if (text !== '' && text !== this.lastClipboard) {
          this.lastClipboard = text
          this.client.sendClipboardSync(text)
        }
      }).catch(() => {
        this.clipboardFailures += 1
        // Do not disable sync until the session reconnects: a temporary
        // permission/focus failure used to stop text clipboard sync for good.
        // Back off to one attempt every five seconds and recover on success.
        if (this.clipboardFailures >= 3) {
          this.clipboardRetryAt = Date.now() + 5000
        }
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

  /// Audio playback belongs to the connection, not to the view: switching to
  /// another tab unmounts the view, and it used to take the player with it,
  /// so the sound stopped and the toggle came back off. Keeping it here lets a
  /// background tab keep playing.
  startAudio(): void {
    if (this.player === null) {
      const format = this.audioFormat
      if (format === null) return
      const player = new PcmPlayer(format.sampleRate, format.channels,
                                   this.audioVolume)
      this.player = player
      void player.start()
    }
    this.client.startAudio()
  }

  stopAudio(): void {
    if (this.player !== null) {
      this.player.stop()
      this.player = null
    }
    this.client.stopAudio()
  }

  audioActive(): boolean {
    return this.player !== null
  }

  getAudioVolume(): number {
    return this.audioVolume
  }

  setAudioVolume(value: number): void {
    this.audioVolume = Math.min(1, Math.max(0, value))
    this.player?.setVolume(this.audioVolume)
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
