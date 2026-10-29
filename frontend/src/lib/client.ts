import {
  FrameFlag,
  PacketFlag,
  PacketType,
  ProtocolError,
  PROTOCOL_VERSION,
  Codec,
  codecBytesPerPixel,
  decodePacket,
  encodePacket,
  isDeflatedCodec,
  parseFramePayload,
  type Frame,
} from './protocol'
import { formatHost } from './net'
import { H264StreamDecoder, h264StreamSupported } from './h264-stream'
import {
  VpxStreamDecoder,
  vpxStreamSupported,
  type VpxCodec,
} from './vpx-stream'
import { WtTransport, fingerprintBytes, webTransportSupported } from './webtransport'

export type ClientPhase = 'idle' | 'connecting' | 'live' | 'error' | 'disconnected'

export interface DisplayInfo {
  index: number
  name: string
  x: number
  y: number
  width: number
  height: number
  primary?: boolean
}

export interface Welcome {
  session: string
  session_id: number
  protocol: number
  service: string
  version: string
  /** Wire protocol: websocket | quic | udp | tcp. */
  transport: string
  /** How the bytes reach the host: direct | p2p | relay. Older cores omit
   *  it; callers fall back to transport (relay is the only legacy mixed-in
   *  value). */
  transport_path?: string
  displays?: DisplayInfo[]
  display_index?: number
  tiles: {
    codec: string
    /** Active quality preset: smooth | balanced | quality. */
    quality?: string
    screen_w: number
    screen_h: number
    tile_w: number
    tile_h: number
    synthetic: boolean
    backend?: string
    /** The capture backend implements set_display_mode. */
    can_set_display_mode?: boolean
  }
  /** Reconnect credential; reused instead of spending another code use. */
  session_token?: string
  /** The host accepts per-second viewer health reports. */
  viewer_stats?: boolean
  /** The host resolves the "auto" quality preset itself. */
  auto_quality?: boolean
  /** The host can offer a WebRTC video track (Phase 4). */
  webrtc?: boolean
  /** The host accepts a WebTransport session on its own port (Phase 5). */
  webtransport?: { port: number; sha256: string }
  /** ICE servers to use for that connection, as "stun:host:port". */
  ice_servers?: string[]
  /** The host drives the frame clock for a client that asked for video_push:
   *  frames arrive on their own and the pull loop stops. */
  video_push?: boolean
  /** e2e: direct TLS to the host; relay: directory WSS only; none: plain. */
  security?: 'e2e' | 'relay' | 'none' | string
  /** Host certificate fingerprint advertised for pinning. */
  tls_fingerprint?: string
  input?: boolean
  input_backend?: string
  clipboard?: boolean
  clipboard_backend?: string
  audio?: boolean
  audio_format?: {
    sample_rate: number
    channels: number
    bits: number
  }
  call?: boolean
  device_name?: string
  device_id?: string
}

export interface CursorShape {
  serial: number
  w: number
  h: number
  hotspotX: number
  hotspotY: number
  rgba: Uint8Array
}

export interface CursorPosition {
  x: number
  y: number
  visible: boolean
}

export interface ChatMessage {
  id: number
  from: 'remote' | 'host' | string
  /** Machine name reported by the sender (or its peer address). */
  senderName?: string
  /** Device ID of the sender, when it has one. */
  senderId?: string
  /** Host-side session id that posted it; 0 for the person at the machine. */
  senderSession?: number
  text: string
  ts: number
}

export interface FileEntry {
  name: string
  dir: boolean
  size: number
  mtime: number
}

/// One file on the host's Explorer clipboard (CF_HDROP).
export interface ClipboardFileEntry {
  name: string
  path: string
  size: number
}

export interface FileMessage {
  action: string
  ok?: boolean
  id?: number
  name?: string
  size?: number
  dir?: string
  path?: string
  root?: string
  chunk?: number
  count?: number
  entries?: FileEntry[]
  message?: string
}

export type SessionEvent =
  | { kind: 'chat'; message: ChatMessage }
  | { kind: 'chat-history'; messages: ChatMessage[] }
  | { kind: 'file'; message: FileMessage }
  | { kind: 'file-chunk'; id: number; offset: number; data: Uint8Array }
  | { kind: 'clipboard-files'; files: ClipboardFileEntry[] }
  | { kind: 'audio'; data: Uint8Array }
  | {
      kind: 'call'
      action: string
      hostMicrophone?: boolean
      startedMs?: number
      message?: string
    }
  | { kind: 'voice'; data: Uint8Array }

export type SessionEventListener = (event: SessionEvent) => void

export interface ClientHandlers {
  onPhase: (phase: ClientPhase, detail?: string) => void
  onWelcome: (welcome: Welcome) => void
  /// `bitmaps` is set for whole-screen stream frames the client already
  /// decoded -- JPEG through the browser's native image decoder, H.264 through
  /// WebCodecs. The sink blits them instead of expanding pixels.
  onFrame: (
    frame: Frame,
    byteLength: number,
    bitmaps?: (ImageBitmap | VideoFrame)[],
  ) => void
  onLatency: (ms: number) => void
  onError: (message: string, code?: string) => void
  /// Live path/protocol changes, e.g. relay switching to ICE. `mode` is the
  /// legacy "relay"/"p2p" flag; `path` and `protocol` are the two axes the UI
  /// labels separately.
  onTransport?: (mode: string, path?: string, protocol?: string) => void
  /**
   * The client core decoded AV1 (wire and decoded byte counts). `fullFrame`
   * means the stream carries only AV1, without a tile image underneath.
   * `wireRate` is the average number of bytes the host actually sent over the
   * network during the last stat window, which is what the user pays for.
   */
  onCodec?: (info: {
    wireBytes: number
    decodedBytes: number
    fullFrame: boolean
    wireRate: number
  }) => void
  /// The host is sending its low-delay H.264 stream for this session. Fired
  /// once per connection so the UI can name the path instead of guessing.
  onStreamVideo?: (codec: string) => void
  /// Another viewer changed the host-global quality preset; this tab must
  /// follow it instead of asserting its own choice.
  onQualityShared?: (mode: string) => void
  onCursor: (shape: CursorShape) => void
  onCursorPosition: (position: CursorPosition) => void
  onClipboard: (text: string) => void
  onSessionEvent?: (event: SessionEvent) => void
  onDisplay?: (index: number) => void
  onAccessRequested?: (id: number) => void
  /**
   * Host capture rate and encoder build time, once per second. The frame-rate
   * controller uses the build time to detect a CPU-bound encoder.
   */
  onCaptureStats?: (info: { fps: number; buildMs: number }) => void
  /** Every FRAME packet, including empty "nothing changed" responses. */
  onWireFrame?: () => void
  /** A live WebRTC video track arrived; the sink paints it into the session
   *  buffer and every existing render/input path keeps working. */
  onRemoteVideo?: (video: HTMLVideoElement) => void
  /** The path the picture actually takes changed: "webrtc" while the track is
   *  live, null when it is gone. The header badge shows it. */
  onVideoPath?: (path: string | null) => void
  /** A frame was dropped because it was stale or overtaken by a newer one. */
  onFrameDropped?: () => void
  /** All quick retries failed; the next automatic attempt is at `atMs`. */
  onRetryScheduled?: (atMs: number) => void
}

interface ControlMessage {
  type?: string
  id?: number
  code?: string
  message?: string
  session?: string
  serial?: number
  w?: number
  h?: number
  hotspotX?: number
  hotspotY?: number
  rgba?: string
  x?: number
  y?: number
  visible?: boolean
  text?: string
  mode?: string
  /// Wire protocol in a `transport` message (websocket | quic | udp | tcp).
  protocol?: string
  index?: number
  action?: string
  ok?: boolean
  av1?: boolean
  wire?: number
  decoded?: number
  /// Host capture rate and encoder build time in a `capture-stats` message.
  fps?: number
  build_ms?: number
  /// Emitted with a codec stat: the stream is AV1-only, not AV1 patches.
  full?: boolean
  /// Bytes per second the host sent over the network in the last window.
  wire_rate?: number
  from?: string
  ts?: number
  name?: string
  size?: number
  dir?: string
  path?: string
  root?: string
  chunk?: number
  entries?: FileEntry[]
  messages?: ChatMessage[]
  sender_name?: string
  sender_id?: string
  sender_session?: number
  files?: ClipboardFileEntry[]
  host_microphone?: boolean
  started_ms?: number
}

/// The wire uses snake_case for the chat sender; the UI works in camelCase.
interface RawChatMessage extends ChatMessage {
  sender_name?: string
  sender_id?: string
  sender_session?: number
}

function toChatMessage(raw: RawChatMessage): ChatMessage {
  return {
    id: raw.id ?? 0,
    from: raw.from ?? 'remote',
    senderName: raw.senderName ?? raw.sender_name,
    senderId: raw.senderId ?? raw.sender_id,
    senderSession: raw.senderSession ?? raw.sender_session,
    text: raw.text ?? '',
    ts: raw.ts ?? 0,
  }
}

function decodeBase64(text: string): Uint8Array | null {
  try {
    const binary = atob(text)
    const out = new Uint8Array(binary.length)
    for (let i = 0; i < binary.length; i += 1) {
      out[i] = binary.charCodeAt(i)
    }
    return out
  } catch {
    return null
  }
}

async function inflateTile(data: Uint8Array): Promise<Uint8Array> {
  const stream = new Blob([data as BlobPart])
    .stream()
    .pipeThrough(new DecompressionStream('deflate'))
  return new Uint8Array(await new Response(stream).arrayBuffer())
}

/// Frame timestamps older than this are ignored when estimating the host clock
/// offset, so a bad estimate or a host clock step recovers within this window.
const kFrameClockWindowMs = 10_000

/// How long a frame request may stay unanswered before the pull loop retries
/// it. ICE/P2P sessions ride UDP, so a request or its reply can be lost; with
/// no retry the session would sit on "waiting for the first frame" forever.
const kFrameRequestTimeoutMs = 1000

/// How often a session without a picture may ask for another keyframe. A large
/// keyframe takes seconds to cross a slow link, so asking faster than that
/// only makes the host encode more of them.
const kKeyframeRecoveryMs = 3000

/// The ICE/P2P handshake rides UDP. If the hello or its welcome is lost the
/// session would never start, so the hello is resent until a welcome arrives.
/// A repeated hello is idempotent on the host (it does not spend another use
/// of the verification code).
const kHelloRetryMs = 2000
const kHelloAttempts = 5

/// A session whose pings go unanswered is dead even if the socket never
/// reports a close: relay and NAT connections go half-open exactly like that,
/// leaving a frozen picture and dead input until the user reconnects.
const kPingIntervalMs = 3000
const kPingMissLimit = 6

/// Unanswered frame requests before the host is treated as unresponsive. The
/// request is retried every second by requestFrame().
const kFrameMissLimit = 15

export class MolinkClient {
  private ws: WebSocket | null = null
  private handlers: Partial<ClientHandlers> = {}
  private pendingFrame = false
  /** The WebTransport session, once it replaced the socket. */
  private wt: WtTransport | null = null
  /** A WebTransport hello is in flight: keep the socket until it answers. */
  private wtPending = false
  /** The page already fell back from WebTransport; do not try it again. */
  private wtDisabled = false
  /** The WebRTC peer for this session, once the host offered one. */
  private rtc: RTCPeerConnection | null = null
  /** Its data channel: control messages prefer it while it is open. */
  private rtcChannel: RTCDataChannel | null = null
  /** WebCodecs decoder for the host's low-delay H.264 stream. */
  private h264 = new H264StreamDecoder()
  /** This browser can decode the H.264 stream (probed once, before hello). */
  private h264Capable = false
  /// The UI has already been told this session is on the H.264 stream.
  private streamCodecNotified = false
  private vp8Capable = false
  private vp9Capable = false
  private vpxDecoders: Partial<Record<VpxCodec, VpxStreamDecoder>> = {}
  /** ICE servers the host advertised for the WebRTC track. */
  private iceServers: string[] = []
  /** The host offered to drive the frame clock for this session. */
  private pushSupported = false
  /** The clock is on right now: the page is visible and pulling is off. */
  private pushVideo = false
  /** Re-evaluates the clock whenever the page is shown or hidden. */
  private visibilityListener: (() => void) | null = null
  /// When the unanswered frame request was sent, for the retry timeout.
  private pendingFrameAt = 0
  private latestFrameSeq = -1
  /// Capture sequence this client has received for each 128 px cell. The
  /// host's per-block sequence map is compared with this to name the cells a
  /// lost or skipped frame left behind.
  private cellReceivedSeq = new Map<string, number>()
  private lastSeqRequest = 0
  private lastKeyframeRequest = 0
  /// Preset this tab pushed; used to notice a host-global preset changed by
  /// another viewer while we keep asking for a colour format.
  private pushedQuality: { mode: string; fps?: number } | null = null
  private lastQualityAssertAt = 0
  /// Last keyframe request made because no picture exists yet.
  private lastKeyframeRecovery = 0
  /// Last tile-clock-driven refresh request, to keep it to one per second.
  private lastClockRequest = 0
  /// Last capture timestamp applied per 128 px screen cell, in microseconds.
  /// A dropped frame only asks for the tiles no newer frame has covered.
  private tileAppliedAt = new Map<string, number>()
  private pendingTiles = new Map<
    string,
    { x: number; y: number; w: number; h: number; ts: number }
  >()
  private lastConnect: {
    host: string
    port: number
    token: string
    requestAccess: boolean
    /// Directory device ID. When set, the first attempt goes through the
    /// local core's /ws/connect ICE proxy instead of a direct WebSocket.
    targetId: string
    /// Skip ICE and keep the session on the relay.
    forceRelay: boolean
    /// This client's machine name and device ID, sent so a shared chat can
    /// label who typed each message. Empty for a plain browser viewer.
    clientName: string
    clientId: string
  } | null = null
  /// The ICE proxy failed; use the resolved address directly from now on.
  private proxyFailed = false
  /// The same-LAN direct socket failed; use the ICE proxy instead.
  private directFailed = false
  /// The typed-IP local proxy failed; use the host's socket directly.
  private directProxyFailed = false
  /// The current socket was opened through /ws/direct on the local core.
  private usedDirectProxy = false
  /// Last application-level error frame. A policy answer (wrong code, locked
  /// host) must not be mistaken for an ICE failure and retried.
  private lastErrorCode = ''
  private lastErrorMessage = ''
  /// Estimated local-minus-host wall-clock difference plus the fastest network
  /// transit, in milliseconds, measured from frame capture timestamps. Frame
  /// freshness must not assume the two machines have synchronized clocks: a
  /// one-second skew used to drop every frame as stale. The pong's `server_ms`
  /// cannot be used for this — the host fills it from a monotonic clock, not
  /// the wall clock.
  private frameClockOffset = 0
  private frameClockSamples: { at: number; offset: number }[] = []
  /// Session token from the last welcome. Reconnecting with it does not spend
  /// another use of the host's temporary verification code.
  private resumeToken = ''
  /// The hello sent on this socket, kept for retries over a lossy ICE path.
  private helloPayload: Record<string, unknown> | null = null
  private helloTimer: number | null = null
  private helloAttempts = 0
  private retryAttempts = 0
  private retryTimer: number | null = null
  private autoRetryTimer: number | null = null
  private refreshTimer: number | null = null
  private pingTimer: number | null = null
  /// When the pending ping was sent, and how many pings in a row went
  /// unanswered. A dead relay link never fires onclose, so this is the only
  /// way to notice it.
  private pendingPingAt = 0
  private missedPongs = 0
  /// Frame requests that timed out in a row, reset by every arriving frame.
  private missedFrameResponses = 0
  private closedByUser = false
  private hasImage = false
  // Decompression is asynchronous; chaining keeps frames applied in order.
  private frameQueue: Promise<void> = Promise.resolve()

  constructor() {
    // One probe, up front: hello goes out on the first connect, and the host
    // only streams H.264 to viewers that asked for it.
    void h264StreamSupported().then((supported) => {
      if (supported === this.h264Capable) return
      this.h264Capable = supported
      // A hello may already have gone out without the flag. Sending it again
      // makes the host re-evaluate which stream codec every viewer can take.
      if (supported && this.isLive() && this.helloPayload !== null) {
        this.sendHello()
      }
    })
    // VP8/VP9 are decoded through WebCodecs too; the host only uses them when
    // every viewer says it can.
    const announce = (codec: VpxCodec, capable: boolean) => {
      if (capable) return
      if (codec === 'vp8') this.vp8Capable = true
      else this.vp9Capable = true
      if (this.isLive() && this.helloPayload !== null) this.sendHello()
    }
    void vpxStreamSupported('vp9').then((ok) => announce('vp9', ok))
    void vpxStreamSupported('vp8').then((ok) => announce('vp8', ok))
  }

  setHandlers(handlers: Partial<ClientHandlers>): void {
    this.handlers = handlers
  }

  connect(host: string, port: number, token: string,
          requestAccess = false, targetId = '', forceRelay = false,
          clientName = '', clientId = ''): void {
    this.disconnect()
    this.stopRetryTimers()
    this.closedByUser = false
    this.retryAttempts = 0
    this.proxyFailed = false
    this.directFailed = false
    this.directProxyFailed = false
    this.lastErrorCode = ''
    this.lastErrorMessage = ''
    this.resumeToken = ''
    this.frameClockOffset = 0
    this.frameClockSamples = []
    this.cellReceivedSeq.clear()
    this.lastSeqRequest = 0
    this.lastConnect = { host, port, token, requestAccess, targetId,
                        forceRelay, clientName, clientId }
    this.openSocket()
  }

  /// Manual reconnect from the disconnected overlay.
  reconnectNow(): void {
    if (this.lastConnect === null) return
    this.stopRetryTimers()
    this.retryAttempts = 0
    this.closedByUser = false
    this.directProxyFailed = false
    // A manual retry starts clean: a stale policy error would otherwise make
    // the next close look like a rejection even when the code is fine.
    this.lastErrorCode = ''
    this.lastErrorMessage = ''
    this.handlers.onPhase?.('connecting')
    this.openSocket()
  }

  private openSocket(): void {
    const args = this.lastConnect
    if (args === null) return
    const {
      host,
      port,
      token,
      requestAccess,
      targetId,
      forceRelay,
      clientName,
      clientId,
    } = args
    this.closedByUser = false
    this.hasImage = false
    this.stopWatchingVisibility()
    this.pushSupported = false
    this.pushVideo = false
    this.pendingFrame = false
    this.pendingFrameAt = 0
    this.missedFrameResponses = 0
    this.latestFrameSeq = -1
    this.lastKeyframeRequest = 0
    this.lastKeyframeRecovery = 0
    this.stopHelloRetry()
    this.tileAppliedAt.clear()
    this.pendingTiles.clear()
    this.frameQueue = Promise.resolve()

    // Two devices on the same LAN should not pay the ICE signaling round
    // trips: the directory only hands out a private address to requesters
    // behind the same NAT, and a direct socket to it opens in milliseconds.
    // If it fails, the ICE proxy below is the fallback. Cross-NAT peers get the
    // public address (or none), so they still start with ICE.
    // Device-ID sessions always use the local core proxy: it owns the AV1
    // decode capability, and a direct LAN socket would look like a tile-only
    // client to the host, disabling AV1 for every session.
    const preferDirect = false
    // A device-ID connection otherwise starts with ICE through the local core.
    // That core also runs the directory client and owns the UDP socket
    // libjuice needs. If the proxy cannot establish a path, fall back to the
    // direct address the directory returned.
    const useProxy = targetId !== '' &&
        (forceRelay || (!this.proxyFailed && !preferDirect))
    // A page served over HTTPS may only open wss (mixed content). The local
    // core proxy is same-origin, so it follows the page scheme; a direct
    // remote host must be the same host (or the local core) to use wss.
    const securePage =
        typeof window !== 'undefined' && window.location.protocol === 'https:'
    const wsScheme = securePage ? 'wss' : 'ws'
    const directSecure =
        securePage &&
        (host === window.location.hostname || host === '127.0.0.1' ||
         host === 'localhost')
    const proxyUrl =
        `${wsScheme}://${window.location.host}/ws/connect?target=${encodeURIComponent(targetId)}` +
        (forceRelay ? '&relay=only' : '')
    // A typed address also goes through the local core so it can decode AV1
    // patches; if that proxy is unavailable (remote UI, older core) the
    // direct socket below still works, just without AV1.
    const useDirectProxy = targetId === '' && !this.directProxyFailed &&
        typeof window !== 'undefined' && window.location.host !== ''
    this.usedDirectProxy = useDirectProxy
    const directProxyUrl =
        `${wsScheme}://${window.location.host}/ws/direct?host=${encodeURIComponent(host)}` +
        `&port=${encodeURIComponent(String(port))}`
    const url = useProxy
      ? proxyUrl
      : useDirectProxy
        ? directProxyUrl
        : `${directSecure ? 'wss' : 'ws'}://${formatHost(host)}:${port}/ws/session`
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
      if (requestAccess || token === '') {
        // No code: ask the host user to approve this session.
        this.sendJson({
          type: 'request-access',
          name: clientName || 'moLink web',
          client: 'molink-web',
          ...(clientId ? { device_id: clientId } : {}),
        })
      } else {
        // Send the code as well: if the host restarted, the resume token is
        // unknown and the code still authenticates in the same round trip.
        this.helloPayload = {
          type: 'hello',
          token,
          // The host then attaches a per-block sequence map to every frame
          // response, which names the cells a lost frame left stale.
          seq_clock: true,
          // Ask the host to drive the frame clock instead of answering a
          // request per frame. The host confirms in `welcome`; until then the
          // pull loop keeps running.
          video_push: true,
          // This browser can decode the host's H.264 stream through WebCodecs;
          // the host picks it when AV1 is not decodable by every viewer.
          h264_stream: this.h264Capable,
          vp9_stream: this.vp9Capable,
          vp8_stream: this.vp8Capable,
          // A browser with WebRTC lets the host send H.264 as a real video
          // track instead of MOLK frames.
          webrtc: typeof RTCPeerConnection !== 'undefined',
          ...(this.resumeToken ? { session: this.resumeToken } : {}),
          ...(clientName ? { name: clientName } : {}),
          ...(clientId ? { device_id: clientId } : {}),
          client: 'molink-web',
          protocol: 1,
        }
        this.sendHello()
      }
    }

    ws.onmessage = (event: MessageEvent) => {
      if (typeof event.data === 'string') {
        this.handleText(event.data)
      } else if (event.data instanceof ArrayBuffer) {
        this.handleBinary(event.data)
      }
    }

    ws.onerror = () => {
      // The close event carries the useful detail; retries are scheduled
      // there so a failed attempt does not flash an error banner.
    }

    ws.onclose = (event: CloseEvent) => {
      // A socket that a stall recovery already replaced must not clear the
      // new one and schedule a second reconnect.
      if (this.ws !== ws) return
      this.stopPing()
      this.stopHelloRetry()
      this.stopWatchingVisibility()
      this.pushSupported = false
      this.pushVideo = false
      this.h264.close()
      for (const decoder of Object.values(this.vpxDecoders)) decoder?.close()
      this.vpxDecoders = {}
      this.streamCodecNotified = false
      this.closeRtc()
      this.pendingFrame = false
      this.ws = null
      // Wrong code / locked host / denied request: the host answered and
      // closed on purpose. Falling back to a direct socket or retrying would
      // just loop; surface the reason so the UI can ask for the code again.
      // The close reason is a second source of truth: over UDP the error
      // frame and the close can race, and the proxy may only deliver the
      // close.
      const closeReason = event.reason ?? ''
      const reasonAuth = /invalid token|unauthorized|locked|too many|banned|access denied/i
          .test(closeReason)
      const policyError = reasonAuth ||
          this.lastErrorCode === 'unauthorized' ||
          this.lastErrorCode === 'locked' ||
          this.lastErrorCode === 'too_many_attempts' ||
          this.lastErrorCode === 'ip_banned' ||
          this.lastErrorCode === 'access_denied';
      if (policyError) {
        this.resumeToken = ''
        if (!this.lastErrorMessage && closeReason) {
          this.lastErrorMessage = closeReason
        }
        this.lastErrorCode = this.lastErrorCode || 'unauthorized'
        this.handlers.onPhase?.('error',
                                this.lastErrorMessage || this.lastErrorCode)
        return
      }
      if (this.usedDirectProxy && !this.closedByUser) {
        // The local proxy could not reach the host (or is unavailable in this
        // page); fall back to the host's own socket, without AV1.
        this.usedDirectProxy = false
        this.directProxyFailed = true
        this.openSocket()
        return
      }
      if (preferDirect && !this.closedByUser) {
        // The same-LAN address did not answer: go through ICE instead.
        this.directFailed = true
        this.openSocket()
        return
      }
      if (useProxy && !this.closedByUser) {
        // ICE failed or the proxy is unavailable: retry the direct path once.
        this.proxyFailed = true
        if (forceRelay) {
          this.handlers.onPhase?.('error', 'the relay is unavailable')
          return
        }
        if (this.directFailed) {
          this.handlers.onPhase?.('error', 'could not reach the device')
          return
        }
        this.openSocket()
        return
      }
      if (this.closedByUser) {
        this.handlers.onPhase?.('idle')
        return
      }
      const reason = event.reason ? `: ${event.reason}` : ''
      // 1000/1008 are deliberate closes (host denied access, session ended).
      if (event.code === 1000 || event.code === 1008) {
        this.handlers.onPhase?.('error', `closed (${event.code})${reason}`)
        return
      }
      this.scheduleRetry(event.code)
    }
  }

  /// One immediate retry, then up to two one-second retries. Only when those
  /// all fail does the UI show a disconnect, followed by one attempt per
  /// minute for as long as the tab stays open.
  private scheduleRetry(code: number): void {
    if (this.retryAttempts < 3) {
      const delay = this.retryAttempts === 0 ? 0 : 1000
      this.retryAttempts += 1
      this.handlers.onPhase?.('connecting')
      this.retryTimer = window.setTimeout(() => {
        this.retryTimer = null
        if (!this.closedByUser) this.openSocket()
      }, delay)
      return
    }
    const deadline = Date.now() + 60000
    this.handlers.onPhase?.('disconnected', `closed (${code})`)
    this.handlers.onRetryScheduled?.(deadline)
    this.autoRetryTimer = window.setTimeout(() => {
      this.autoRetryTimer = null
      if (this.closedByUser) return
      this.handlers.onPhase?.('connecting')
      this.openSocket()
    }, 60000)
  }

  private stopRetryTimers(): void {
    if (this.retryTimer !== null) {
      window.clearTimeout(this.retryTimer)
      this.retryTimer = null
    }
    if (this.autoRetryTimer !== null) {
      window.clearTimeout(this.autoRetryTimer)
      this.autoRetryTimer = null
    }
  }

  disconnect(): void {
    this.closedByUser = true
    this.stopRetryTimers()
    this.stopPing()
    this.stopHelloRetry()
    this.stopWatchingVisibility()
    this.pushSupported = false
    this.pushVideo = false
    const ws = this.ws
    this.ws = null
    if (ws && ws.readyState <= WebSocket.OPEN) {
      ws.close(1000, 'client closing')
    }
  }

  isLive(): boolean {
    if (this.wt !== null && this.wt.isOpen) return true
    return this.ws !== null && this.ws.readyState === WebSocket.OPEN
  }

  /// Records a frame's capture time so frame ages can be measured on the
  /// host's clock. The host stamps frames with its wall clock, so the client
  /// cannot subtract them from its own `Date.now()` directly. The fastest
  /// frame in the window is the one least delayed by the network, so the
  /// minimum arrival-minus-capture is the best available skew estimate.
  private noteFrameClock(tsUs: number): void {
    if (!Number.isFinite(tsUs) || tsUs <= 0) return
    const now = Date.now()
    const samples = this.frameClockSamples
    samples.push({ at: now, offset: now - tsUs / 1000 })
    const cutoff = now - kFrameClockWindowMs
    let first = 0
    while (first < samples.length && samples[first].at < cutoff) first += 1
    if (first > 0) samples.splice(0, first)
    let offset = samples[0].offset
    for (const sample of samples) {
      if (sample.offset < offset) offset = sample.offset
    }
    this.frameClockOffset = offset
  }

  /// How much longer this frame took than the fastest frame in the window,
  /// in milliseconds. Negative values mean it was faster than the minimum.
  private frameAgeMs(tsUs: number): number {
    return Date.now() - this.frameClockOffset - tsUs / 1000
  }

  /// Sends the hello and keeps resending it until a welcome arrives. The ICE
  /// path is UDP, so either direction can be lost without notice.
  private sendHello(): void {
    if (this.helloPayload === null || !this.isLive()) return
    this.sendJson(this.helloPayload)
    this.helloAttempts += 1
    if (this.helloTimer !== null) return
    this.helloTimer = window.setInterval(() => {
      if (this.helloPayload === null || this.helloAttempts >= kHelloAttempts) {
        this.stopHelloRetry()
        return
      }
      this.sendHello()
    }, kHelloRetryMs)
  }

  private stopHelloRetry(): void {
    if (this.helloTimer !== null) {
      window.clearInterval(this.helloTimer)
      this.helloTimer = null
    }
    this.helloPayload = null
    this.helloAttempts = 0
  }

  /// Turns the host clock on for a visible page and off for a hidden one.
  ///
  /// Hiding a tab (or losing focus, which WebKitGTK reports the same way)
  /// used to drop the session to a 1 Hz pull; a pushed session would otherwise
  /// keep receiving full-rate frames for a page nobody is looking at. Turning
  /// the clock off hands the session back to its own slow pull instead.
  private watchVisibility(): void {
    if (this.visibilityListener !== null) return
    const apply = () => {
      const on = this.pushSupported && document.visibilityState === 'visible'
      if (on === this.pushVideo) return
      this.pushVideo = on
      if (this.pushSupported) this.sendJson({ type: 'video-push', on })
    }
    this.visibilityListener = apply
    document.addEventListener('visibilitychange', apply)
    apply()
  }

  private stopWatchingVisibility(): void {
    if (this.visibilityListener === null) return
    document.removeEventListener('visibilitychange', this.visibilityListener)
    this.visibilityListener = null
  }

  /** Asks for one synthetic frame. Returns false while one is in flight. */
  requestFrame(): boolean {
    // The host owns the clock for this session: polling would make it send
    // every frame twice.
    if (this.pushVideo) return false
    if (!this.isLive()) return false
    if (this.pendingFrame) {
      if (Date.now() - this.pendingFrameAt < kFrameRequestTimeoutMs) {
        return false
      }
      // The previous retry was not answered either. A host that keeps pongs
      // flowing but stops answering frame pulls has a stuck capture/input
      // path; reconnecting is the only client-side recovery.
      this.missedFrameResponses += 1
      if (this.missedFrameResponses >= kFrameMissLimit) {
        this.recoverStalledSession('the host stopped sending frames')
        return false
      }
    }
    this.pendingFrame = true
    this.pendingFrameAt = Date.now()
    this.sendJson({ type: 'request-frame' })
    return true
  }

  /// Brings up the peer the host offered and answers with our SDP.
  private async acceptRtcOffer(sdp: string): Promise<void> {
    if (sdp === '' || typeof RTCPeerConnection === 'undefined') return
    this.closeRtc()
    const pc = new RTCPeerConnection({
      iceServers: (this.iceServers ?? []).map((url) => ({ urls: url })),
    })
    this.rtc = pc
    pc.onicecandidate = (event) => {
      if (event.candidate === null) return
      this.sendJson(
        {
          type: 'rtc-ice',
          candidate: event.candidate.candidate,
          mid: event.candidate.sdpMid ?? '',
        },
        true,
      )
    }
    pc.ontrack = (event) => {
      const stream = event.streams[0] ?? new MediaStream([event.track])
      // The sink paints this into the session buffer; a detached element is
      // enough and never fights the canvas for layout.
      const video = document.createElement('video')
      video.autoplay = true
      video.muted = true
      video.playsInline = true
      video.srcObject = stream
      void video.play().catch(() => undefined)
      this.handlers.onRemoteVideo?.(video)
      // The picture no longer travels as MOLK frames: say so, or the header
      // would keep claiming the WebSocket path.
      this.handlers.onVideoPath?.('webrtc')
    }
    pc.ondatachannel = (event) => {
      // The host opens two channels: "molink" for control and "audio" for the
      // realtime audio packets, which are unreliable and therefore kept off
      // the control channel.
      if (event.channel.label === 'file') {
        // File transfers have their own reliable channel, so they never sit
        // in front of control messages.
        event.channel.onmessage = (message: MessageEvent) => {
          if (message.data instanceof ArrayBuffer) this.handleBinary(message.data)
        }
        return
      }
      if (event.channel.label === 'audio') {
        event.channel.onmessage = (message: MessageEvent) => {
          if (message.data instanceof ArrayBuffer) this.handleBinary(message.data)
        }
        return
      }
      this.rtcChannel = event.channel
      event.channel.onmessage = (message: MessageEvent) => {
        if (typeof message.data === 'string') this.handleText(message.data)
      }
      event.channel.onclose = () => {
        if (this.rtcChannel === event.channel) this.rtcChannel = null
      }
    }
    try {
      await pc.setRemoteDescription({ type: 'offer', sdp })
      const answer = await pc.createAnswer()
      await pc.setLocalDescription(answer)
      this.sendJson({ type: 'rtc-answer', sdp: answer.sdp ?? '' }, true)
    } catch {
      this.closeRtc()
    }
  }

  private closeRtc(): void {
    this.handlers.onVideoPath?.(null)
    if (this.rtcChannel !== null) {
      try {
        this.rtcChannel.close()
      } catch {
        // already closed
      }
      this.rtcChannel = null
    }
    if (this.rtc !== null) {
      try {
        this.rtc.close()
      } catch {
        // already closed
      }
      this.rtc = null
    }
  }

  sendInput(payload: Record<string, unknown>): void {
    this.sendJson({ type: 'input', ...payload })
  }

  /** Asks for the next capture to be a full keyframe (resync after a loss). */
  requestKeyframe(): void {
    this.sendJson({ type: 'request-keyframe' })
  }

  /**
   * Reports what this viewer's own pipeline achieved in the last second: the
   * host uses it to send less detail when the client, not its encoder, is the
   * limit. Ignored by hosts that did not advertise `viewer_stats`.
   */
  sendViewerStats(fps: number, dropped: number, rxRate: number): void {
    if (!this.isLive()) return
    this.sendJson({
      type: 'viewer',
      fps: Math.max(0, Math.round(fps)),
      dropped: Math.max(0, Math.round(dropped)),
      rx: Math.max(0, Math.round(rxRate)),
    })
  }

  /** Asks the core to change its capture rate (global for this host). */
  setCaptureFps(fps: number): void {
    if (!this.isLive()) return
    this.sendJson({ type: 'capture-fps', fps })
  }

  /**
   * Applies an encoding preset. "auto" is resolved client-side, so only the
   * three concrete modes are ever sent.
   */
  setQuality(mode: string, fps?: number): void {
    this.pushedQuality = { mode, fps }
    this.lastQualityAssertAt = 0
    if (!this.isLive()) return
    this.sendJson({ type: 'quality', mode, ...(fps ? { fps } : {}) })
  }

  /** Switches the remote capture to another monitor. */
  setDisplay(index: number): void {
    if (!this.isLive()) return
    this.sendJson({ type: 'display', index })
  }

  /** Asks the host driver for a different display mode. */
  setDisplayMode(width: number, height: number): void {
    if (!this.isLive()) return
    this.sendJson({ type: 'display-mode', width, height })
  }

  requestClipboard(): void {
    this.sendJson({ type: 'clipboard', action: 'get' })
  }

  requestClipboardFiles(): void {
    if (!this.isLive()) return
    this.sendJson({ type: 'clipboard', action: 'files' })
  }

  startAudio(): void {
    if (!this.isLive()) return
    this.sendJson({ type: 'audio', action: 'start' })
  }

  stopAudio(): void {
    if (!this.isLive()) return
    this.sendJson({ type: 'audio', action: 'stop' })
  }

  inviteCall(): void {
    if (!this.isLive()) return
    this.sendJson({ type: 'call', action: 'invite' })
  }

  hangupCall(): void {
    if (!this.isLive()) return
    this.sendJson({ type: 'call', action: 'hangup' })
  }

  sendVoice(data: Uint8Array): void {
    if (!this.isLive() || this.ws === null) return
    const packet = encodePacket({
      version: PROTOCOL_VERSION,
      type: PacketType.voice,
      flags: 0,
      session: 0,
      seq: 0,
      tsUs: Date.now() * 1000,
      payload: data,
    })
    this.sendRawPacket(packet)
  }

  setClipboard(text: string): void {
    this.sendJson({ type: 'clipboard', action: 'set', text })
  }

  requestChatHistory(): void {
    this.sendJson({ type: 'chat', action: 'history' })
  }

  sendChat(text: string): void {
    if (!this.isLive()) return
    this.sendJson({ type: 'chat', action: 'send', text })
  }

  sendFileAction(
    action: string,
    extra: Record<string, unknown> = {},
  ): void {
    if (!this.isLive()) return
    this.sendJson({ type: 'file', action, ...extra })
  }

  sendFileChunk(id: number, offset: number, data: Uint8Array): void {
    if (!this.isLive() || this.ws === null) return
    const payload = new Uint8Array(16 + data.byteLength)
    const view = new DataView(payload.buffer)
    view.setUint32(0, id, true)
    view.setUint32(4, 0, true)
    view.setBigUint64(8, BigInt(offset), true)
    payload.set(data, 16)
    const packet = encodePacket({
      version: PROTOCOL_VERSION,
      type: PacketType.fileData,
      flags: 0,
      session: 0,
      seq: 0,
      tsUs: Date.now() * 1000,
      payload,
    })
    this.sendRawPacket(packet)
  }

  /** Real-time clipboard update; the core forwards it to other sessions. */
  sendClipboardSync(text: string): void {
    if (!this.isLive() || text === '') return
    let hash = 2166136261
    for (let i = 0; i < text.length; i += 1) {
      hash ^= text.charCodeAt(i)
      hash = Math.imul(hash, 16777619)
    }
    this.sendJson({
      type: 'clipboard-sync',
      text,
      hash: (hash >>> 0).toString(16),
      chain: '',
    })
  }

  private handleText(text: string): void {
    this.noteInbound()
    let message: ControlMessage
    try {
      message = JSON.parse(text) as ControlMessage
    } catch {
      this.handlers.onError?.('control message was not valid JSON')
      return
    }

    switch (message.type) {
      case 'welcome': {
        const welcome = message as unknown as Welcome
        if (typeof welcome.session_token === 'string' &&
            welcome.session_token !== '') {
          this.resumeToken = welcome.session_token
        }
        this.stopHelloRetry()
        // A host that answered `video_push` sends frames on its own clock
        // while this page is the one being looked at; the pull loop stays
        // quiet meanwhile so frames are not sent twice.
        this.pushSupported = welcome.video_push === true
        this.iceServers = welcome.ice_servers ?? []
        if (this.wtPending && this.wt !== null) {
          // This welcome came over the WebTransport session: it is up, and the
          // socket has done its job (signaling and the certificate hash).
          this.wtPending = false
          this.retireSocket()
        } else if (this.lastConnect !== null) {
          void this.startWebTransport(
            welcome,
            this.lastConnect.host,
            this.lastConnect.token,
          )
        }
        this.watchVisibility()
        this.handlers.onWelcome?.(welcome)
        this.retryAttempts = 0
        this.stopRetryTimers()
        this.handlers.onPhase?.('live')
        this.startPing()
        // Ask for the first picture immediately. WebKitGTK reports the page
        // as hidden while its window is unfocused, so waiting for the visible
        // pull loop would leave a freshly opened session on "waiting for the
        // first frame" until the user clicks the window. A pushed session
        // gets its first picture from the host's clock instead.
        this.requestKeyframe()
        this.requestFrame()
        break
      }
      case 'rtc-offer':
        void this.acceptRtcOffer((message as unknown as { sdp?: string }).sdp ?? '')
        break
      case 'rtc-ice': {
        const ice = message as unknown as { candidate?: string; mid?: string }
        if (this.rtc && typeof ice.candidate === 'string' &&
            ice.candidate !== '') {
          void this.rtc
            .addIceCandidate({ candidate: ice.candidate, sdpMid: ice.mid ?? '' })
            .catch(() => undefined)
        }
        break
      }
      case 'hello_required':
        // The host asks for authentication; resend in case the first hello
        // was lost on the UDP path (a duplicate hello is harmless).
        this.sendHello()
        break
      case 'access-requested':
        this.handlers.onAccessRequested?.(message.id ?? 0)
        break
      case 'display':
        if (typeof message.index === 'number') {
          this.handlers.onDisplay?.(message.index)
        }
        break
      case 'pong': {
        this.pendingPingAt = 0
        this.missedPongs = 0
        const sent = (message as { t?: number }).t
        if (typeof sent === 'number') {
          this.handlers.onLatency?.(Math.max(0, performance.now() - sent))
        }
        break
      }
      case 'ping':
        this.sendJson({ type: 'pong', t: (message as { t?: number }).t })
        break
      case 'cursor': {
        const w = message.w ?? 0
        const h = message.h ?? 0
        const rgba = message.rgba ? decodeBase64(message.rgba) : null
        if (
          rgba &&
          w > 0 &&
          h > 0 &&
          rgba.byteLength === w * h * 4 &&
          typeof message.serial === 'number'
        ) {
          this.handlers.onCursor?.({
            serial: message.serial,
            w,
            h,
            hotspotX: message.hotspotX ?? 0,
            hotspotY: message.hotspotY ?? 0,
            rgba,
          })
        }
        break
      }
      case 'cursor-position':
        this.handlers.onCursorPosition?.({
          x: message.x ?? 0,
          y: message.y ?? 0,
          visible: message.visible !== false,
        })
        break
      case 'clipboard':
        if (message.action === 'files') {
          this.handlers.onSessionEvent?.({
            kind: 'clipboard-files',
            files: message.files ?? [],
          })
        } else {
          this.handlers.onClipboard?.(message.text ?? '')
        }
        break
      case 'clipboard-sync':
        this.handlers.onClipboard?.(message.text ?? '')
        break
      case 'chat':
        if (message.action === 'history') {
          this.handlers.onSessionEvent?.({
            kind: 'chat-history',
            messages: (message.messages ?? []).map((item) =>
              toChatMessage(item as RawChatMessage),
            ),
          })
        } else if (message.action === 'message' && message.text) {
          this.handlers.onSessionEvent?.({
            kind: 'chat',
            message: toChatMessage({
              id: message.id ?? 0,
              from: message.from ?? 'remote',
              sender_name: message.sender_name,
              sender_id: message.sender_id,
              sender_session: message.sender_session,
              text: message.text,
              ts: message.ts ?? Date.now(),
            } as RawChatMessage),
          })
        }
        break
      case 'file':
        this.handlers.onSessionEvent?.({
          kind: 'file',
          message: message as unknown as FileMessage,
        })
        break
      case 'call':
        this.handlers.onSessionEvent?.({
          kind: 'call',
          action: message.action ?? '',
          hostMicrophone: message.host_microphone,
          startedMs: message.started_ms,
          message: message.message,
        })
        break
      case 'capture-stats':
        this.handlers.onCaptureStats?.({
          fps: typeof message.fps === 'number' ? message.fps : 0,
          buildMs: typeof message.build_ms === 'number' ? message.build_ms : 0,
        })
        break
      case 'quality':
        if (typeof message.mode === 'string') {
          this.handlers.onQualityShared?.(message.mode)
        }
        break
      case 'codec':
        if (message.av1 === true) {
          this.handlers.onCodec?.({
            wireBytes: typeof message.wire === 'number' ? message.wire : 0,
            decodedBytes:
              typeof message.decoded === 'number' ? message.decoded : 0,
            fullFrame: message.full === true,
            wireRate:
              typeof message.wire_rate === 'number' ? message.wire_rate : 0,
          })
        }
        break
      case 'transport':
        if (typeof message.mode === 'string') {
          this.handlers.onTransport?.(
            message.mode,
            typeof message.path === 'string' ? message.path : undefined,
            typeof message.protocol === 'string' ? message.protocol : undefined,
          )
        }
        break
      case 'error': {
        const code = typeof message.code === 'string' ? message.code : ''
        // The host rejected our credentials (wrong or exhausted code); a
        // fresh code will arrive from the UI, so drop the stale resume token.
        if (code === 'unauthorized' || code === 'ip_banned' ||
            code === 'too_many_attempts') {
          this.resumeToken = ''
        }
        this.lastErrorCode = code
        this.lastErrorMessage =
            typeof message.message === 'string' ? message.message : code
        if (/hello/i.test(this.lastErrorMessage)) {
          // A request outraced our hello (a UDP reordering/loss race): send
          // it again instead of failing the whole session.
          this.sendHello()
        } else {
          this.stopHelloRetry()
        }
        this.handlers.onError?.(
          this.lastErrorMessage || code || 'error', code || undefined)
        break
      }
      default:
        break
    }
  }

  private handleBinary(buffer: ArrayBuffer): void {
    this.noteInbound()
    try {
      const packet = decodePacket(buffer)
      if (packet.type === PacketType.audio) {
        this.handlers.onSessionEvent?.({
          kind: 'audio',
          data: packet.payload,
        })
        return
      }
      if (packet.type === PacketType.voice) {
        this.handlers.onSessionEvent?.({
          kind: 'voice',
          data: packet.payload,
        })
        return
      }
      if (packet.type === PacketType.fileData) {
        const payload = packet.payload
        if (payload.byteLength >= 16) {
          const view = new DataView(
            payload.buffer,
            payload.byteOffset,
            payload.byteLength,
          )
          this.handlers.onSessionEvent?.({
            kind: 'file-chunk',
            id: view.getUint32(0, true),
            offset: Number(view.getBigUint64(8, true)),
            data: payload.subarray(16),
          })
        }
        return
      }
      if (packet.type === PacketType.tileClock) {
        this.handleTileClock(packet.payload)
        return
      }
      if (packet.type === PacketType.tileSeq) {
        // The whole block map arrives as one packet, usually deflated. Handle
        // it on the frame queue so it is compared after the frames it
        // describes have been received.
        if ((packet.flags & PacketFlag.compressed) !== 0) {
          const payload = packet.payload
          this.frameQueue = this.frameQueue
            .then(async () => {
              this.handleTileSeq(await inflateTile(payload))
            })
            .catch(() => {})
        } else {
          this.handleTileSeq(packet.payload)
        }
        return
      }
      if (packet.type !== PacketType.frame) return
      this.pendingFrame = false
      this.missedFrameResponses = 0
      const frame = parseFramePayload(packet.payload)
      this.noteFrameClock(packet.tsUs)
      this.noteReceived(frame, (packet.flags & PacketFlag.tileRefresh) !== 0)
      this.reconcileQualityPreset(frame)
      if (frame.codec === Codec.jpeg) {
        // Stream frames: one whole screen per packet, decoded natively by the
        // browser and blitted. They arrive as tile refreshes, so they carry
        // their own base image and never depend on the delta chain.
        if ((frame.flags & FrameFlag.keyframe) !== 0) this.hasImage = true
        if (frame.tiles.length === 0) return
        if (!this.hasImage) {
          this.requestKeyframeThrottled()
          return
        }
        if (this.frameAgeMs(packet.tsUs) > 1000) {
          this.handlers.onFrameDropped?.()
          return
        }
        this.handlers.onWireFrame?.()
        if (frame.frameSeq > this.latestFrameSeq) {
          this.latestFrameSeq = frame.frameSeq
        }
        // The one-second timestamp clock compares against what was painted;
        // without this every cell would look stale after a JPEG frame and the
        // page would ask for tile refreshes it already has.
        this.noteApplied(frame, packet.tsUs)
        const tiles = frame.tiles
        this.frameQueue = this.frameQueue
          .then(async () => {
            const bitmaps = await Promise.all(
              tiles.map(async (tile) => {
                // A copy gives the Blob a plain ArrayBuffer (no shared one).
                const bytes = new Uint8Array(tile.data)
                return createImageBitmap(new Blob([bytes], { type: 'image/jpeg' }))
              }),
            )
            try {
              this.handlers.onFrame?.(frame, buffer.byteLength, bitmaps)
            } finally {
              for (const bitmap of bitmaps) bitmap.close()
            }
          })
          .catch(() => undefined)
        return
      }
      if (
        frame.codec === Codec.h264Stream ||
        frame.codec === Codec.vp8Stream ||
        frame.codec === Codec.vp9Stream
      ) {
        // Low-delay stream: one whole screen per packet, decoded by WebCodecs
        // (H.264 as Annex-B, VP8/VP9 as raw codec frames). Like the AV1 stream
        // it only decodes on top of the previous picture, so nothing is
        // painted until a keyframe arrived.
        if ((frame.flags & FrameFlag.keyframe) !== 0) this.hasImage = true
        if (frame.tiles.length === 0) return
        if (!this.hasImage) {
          this.requestKeyframeThrottled()
          return
        }
        if (this.frameAgeMs(packet.tsUs) > 1000) {
          this.handlers.onFrameDropped?.()
          return
        }
        this.handlers.onWireFrame?.()
        if (frame.frameSeq > this.latestFrameSeq) {
          this.latestFrameSeq = frame.frameSeq
        }
        this.noteApplied(frame, packet.tsUs)
        if (!this.streamCodecNotified) {
          this.streamCodecNotified = true
          this.handlers.onStreamVideo?.(
            frame.codec === Codec.h264Stream
              ? 'h264'
              : frame.codec === Codec.vp9Stream
                ? 'vp9'
                : 'vp8',
          )
        }
        const keyframe = (frame.flags & FrameFlag.keyframe) !== 0
        // A copy: WebCodecs keeps the buffer until the chunk is consumed, and
        // the packet's view belongs to the caller.
        const accessUnit = new Uint8Array(frame.tiles[0]!.data)
        // Feed the decoder without waiting for the previous picture. WebKit's
        // decoder only emits a picture once the next chunk (or a flush)
        // arrives, so awaiting each output before submitting the next chunk
        // deadlocks after the first frame: one chunk goes in, nothing comes
        // out, and the queue never moves again. Output order follows input
        // order, so painting as promises resolve keeps the picture ordered.
        const decoder =
          frame.codec === Codec.h264Stream
            ? this.h264
            : this.vpxDecoder(frame.codec === Codec.vp9Stream ? 'vp9' : 'vp8')
        this.frameQueue = this.frameQueue
          .then(() => {
            void decoder
              .decode(accessUnit, keyframe)
              .then((picture) => {
                if (picture === null) {
                  // Behind, broken, or a delta with no reference: only a
                  // keyframe brings the stream back.
                  this.requestKeyframeThrottled()
                  return
                }
                try {
                  this.handlers.onFrame?.(frame, buffer.byteLength, [picture])
                } finally {
                  picture.close()
                }
              })
              .catch(() => undefined)
          })
          .catch(() => undefined)
        return
      }
      if ((packet.flags & PacketFlag.tileRefresh) !== 0) {
        // A tile refresh carries exact pixels, so it can establish the first
        // picture: the AV1 stream sends everything as a refresh, and without
        // this the page would refuse every frame of a fresh stream and stay
        // frozen while asking for a keyframe that also arrives as a refresh.
        if ((frame.flags & FrameFlag.keyframe) !== 0) this.hasImage = true
        if (!this.hasImage) {
          this.requestKeyframeThrottled()
          return
        }
        if (isDeflatedCodec(frame.codec)) {
          this.frameQueue = this.frameQueue
            .then(() =>
              this.deliverCompressed(frame, buffer.byteLength, packet.tsUs,
                                     true),
            )
            .catch((error: unknown) => {
              this.handlers.onError?.(
                error instanceof Error ? error.message : 'tile refresh failed',
              )
            })
        } else {
          this.deliver(frame, buffer.byteLength, packet.tsUs, true)
        }
        return
      }
      const keyframe = (frame.flags & FrameFlag.keyframe) !== 0
      // A frame that spent more than a second in flight is worse than a
      // skipped one: showing it late causes visible lag. Drop it and ask for
      // a fresh keyframe instead. Keyframes are exempt: a large first
      // keyframe can take seconds over a slow or lossy link, and dropping it
      // would leave the session with no picture at all.
      if (!keyframe && this.frameAgeMs(packet.tsUs) > 1000) {
        this.handlers.onFrameDropped?.()
        this.queueTileRefresh(frame, packet.tsUs)
        return
      }
      if (frame.frameSeq > this.latestFrameSeq) {
        this.latestFrameSeq = frame.frameSeq
      } else if (!keyframe) {
        // A newer frame is already queued; this delta would apply on the
        // wrong base image.
        this.handlers.onFrameDropped?.()
        this.queueTileRefresh(frame, packet.tsUs)
        return
      }
      // Count pulls that completed, even when the screen did not change. The
      // auto quality controller uses this as a congestion signal.
      this.handlers.onWireFrame?.()

      if (isDeflatedCodec(frame.codec)) {
        this.frameQueue = this.frameQueue
          .then(() =>
            this.deliverCompressed(frame, buffer.byteLength, packet.tsUs),
          )
          .catch((error: unknown) => {
            this.handlers.onError?.(
              error instanceof Error
                ? error.message
                : 'tile decompression failed',
            )
          })
        return
      }

      this.deliver(frame, buffer.byteLength, packet.tsUs)
    } catch (error) {
      this.pendingFrame = false
      const message =
        error instanceof ProtocolError ? error.message : 'could not decode frame'
      this.handlers.onError?.(message)
    }
  }

  /**
   * The host's capture preset is global across sessions, so another viewer can
   * switch it to turbo (gray8) while this tab asked for a colour preset. The
   * tiles that arrive then are monochrome and would stay that way until their
   * pixels change. Re-assert this tab's preset and pull one full frame.
   */
  private reconcileQualityPreset(frame: Frame): void {
    const pushed = this.pushedQuality
    if (!pushed || pushed.mode === 'turbo') return
    if (frame.codec !== Codec.gray8 && frame.codec !== Codec.deflateGray8) {
      return
    }
    const now = Date.now()
    if (now - this.lastQualityAssertAt < 2000) return
    this.lastQualityAssertAt = now
    this.sendJson({
      type: 'quality',
      mode: pushed.mode,
      ...(pushed.fps ? { fps: pushed.fps } : {}),
    })
    this.requestKeyframe()
  }

  private async deliverCompressed(
    frame: Frame,
    byteLength: number,
    tsUs: number,
    refresh = false,
  ): Promise<void> {
    if (!refresh && (frame.flags & FrameFlag.keyframe) === 0) {
      if (frame.frameSeq < this.latestFrameSeq ||
          this.frameAgeMs(tsUs) > 1000) {
        this.handlers.onFrameDropped?.()
        this.queueTileRefresh(frame, tsUs)
        return
      }
    }
    const bytesPerPixel = codecBytesPerPixel(frame.codec)
    if (bytesPerPixel === 0) throw new Error('unsupported deflate codec')
    const tiles = await Promise.all(
      frame.tiles.map(async (tile) => {
        const data = await inflateTile(tile.data)
        if (data.byteLength !== tile.w * tile.h * bytesPerPixel) {
          throw new Error('tile size mismatch after decompression')
        }
        return { ...tile, data }
      }),
    )
    this.deliver({ ...frame, tiles }, byteLength, tsUs, refresh)
  }

  private deliver(frame: Frame, byteLength: number, tsUs: number,
                  refresh = false): void {
    if (!refresh) {
      if ((frame.flags & FrameFlag.keyframe) !== 0) {
        this.hasImage = true
      } else if (!this.hasImage) {
        // A delta cannot be drawn over an image that does not exist yet. The
        // server requests a keyframe on our behalf; the next frames will carry
        // it. If that keyframe was lost on the UDP path, ask again, but leave
        // it time to arrive instead of piling up more encodes.
        const now = performance.now()
        if (now - this.lastKeyframeRecovery >= kKeyframeRecoveryMs) {
          this.lastKeyframeRecovery = now
          this.requestKeyframe()
        }
        return
      }
    }
    // A keyframe with no tiles would mean an empty screen; nothing to draw.
    if (frame.tiles.length === 0) return

    // A repair (tile refresh) snapshots the host's current pixels. A delta
    // that was already in flight when the repair was produced has an older
    // capture timestamp, and painting it would put the stale pixels back.
    // Skip any tile whose cells already hold newer pixels; the tile clock
    // repairs the skipped cells if they are genuinely behind.
    let tiles = frame.tiles
    if (!refresh) {
      tiles = frame.tiles.filter((tile) =>
        !this.tileCellKeys(tile.x, tile.y, tile.w, tile.h).some(
          (key) => (this.tileAppliedAt.get(key) ?? 0) > tsUs,
        ),
      )
      if (tiles.length === 0) return
    }
    const painted = tiles === frame.tiles ? frame : { ...frame, tiles }

    this.noteApplied(painted, tsUs)
    this.handlers.onFrame?.(painted, byteLength)
  }

  /// The host's per-cell freshness map. Cells whose newest pixels are more
  /// than a second newer than what this client applied were missed on the
  /// wire; ask for them again. A lost tile refresh therefore repairs itself
  /// on the next clock instead of leaving a stale rectangle on screen.
  private handleTileClock(payload: Uint8Array): void {
    if (payload.byteLength < 8) return
    const view = new DataView(payload.buffer, payload.byteOffset,
                              payload.byteLength)
    const cols = view.getUint16(0, true)
    const rows = view.getUint16(2, true)
    const cell = view.getUint16(4, true)
    if (cols === 0 || rows === 0 || cell !== 128) return
    if (payload.byteLength < 8 + cols * rows * 8) return
    if (!this.hasImage) {
      this.requestKeyframeThrottled()
      return
    }
    const now = performance.now()
    if (now - this.lastClockRequest < 1000) return
    const tiles: Array<{ x: number; y: number; w: number; h: number }> = []
    for (let row = 0; row < rows; row += 1) {
      let runStart = -1
      for (let col = 0; col <= cols; col += 1) {
        let staleCell = false
        if (col < cols) {
          const hostTs = Number(
            view.getBigUint64(8 + (row * cols + col) * 8, true),
          )
          const applied = this.tileAppliedAt.get(`${col},${row}`) ?? 0
          staleCell = hostTs > applied + 1_000_000
        }
        if (staleCell && runStart < 0) runStart = col
        if (!staleCell && runStart >= 0) {
          tiles.push({
            x: runStart * cell,
            y: row * cell,
            w: (col - runStart) * cell,
            h: cell,
          })
          runStart = -1
        }
      }
    }
    if (tiles.length === 0) return
    this.lastClockRequest = now
    this.sendJson({ type: 'request-tiles', tiles: tiles.slice(0, 64) })
  }

  /// Names the cells this client missed: the host reports the capture
  /// sequence of every 128 px cell, and anything newer than what was received
  /// is asked for at once. Unlike the timestamp clock there is no one-second
  /// window, so a lost frame is repaired on the next frame response.
  private handleTileSeq(payload: Uint8Array): void {
    if (payload.byteLength < 8) return
    const view = new DataView(payload.buffer, payload.byteOffset,
                              payload.byteLength)
    const cols = view.getUint16(0, true)
    const rows = view.getUint16(2, true)
    const cell = view.getUint16(4, true)
    if (cols === 0 || rows === 0 || cell !== 128) return
    if (payload.byteLength < 8 + cols * rows * 4) return
    if (!this.hasImage) {
      this.requestKeyframeThrottled()
      return
    }
    const now = performance.now()
    if (now - this.lastSeqRequest < 100) return
    const tiles: Array<{ x: number; y: number; w: number; h: number }> = []
    for (let row = 0; row < rows; row += 1) {
      let runStart = -1
      for (let col = 0; col <= cols; col += 1) {
        let behind = false
        if (col < cols) {
          const hostSeq = view.getUint32(8 + (row * cols + col) * 4, true)
          const mine = this.cellReceivedSeq.get(`${col},${row}`)
          behind = mine === undefined ? hostSeq > 0 : hostSeq > mine
        }
        if (behind && runStart < 0) runStart = col
        if (!behind && runStart >= 0) {
          tiles.push({
            x: runStart * cell,
            y: row * cell,
            w: (col - runStart) * cell,
            h: cell,
          })
          runStart = -1
        }
      }
    }
    if (tiles.length === 0) return
    this.lastSeqRequest = now
    this.sendJson({ type: 'request-tiles', tiles: tiles.slice(0, 64) })
  }

  private tileCellKeys(x: number, y: number, w: number, h: number): string[] {
    const cell = 128
    const keys: string[] = []
    const firstX = Math.floor(x / cell)
    const firstY = Math.floor(y / cell)
    const lastX = Math.floor((x + Math.max(1, w) - 1) / cell)
    const lastY = Math.floor((y + Math.max(1, h) - 1) / cell)
    for (let cy = firstY; cy <= lastY; ++cy) {
      for (let cx = firstX; cx <= lastX; ++cx) {
        keys.push(`${cx},${cy}`)
      }
    }
    return keys
  }

  private noteApplied(frame: Frame, tsUs: number): void {
    for (const tile of frame.tiles) {
      for (const key of this.tileCellKeys(tile.x, tile.y, tile.w, tile.h)) {
        const previous = this.tileAppliedAt.get(key)
        if (previous === undefined || tsUs > previous) {
          this.tileAppliedAt.set(key, tsUs)
        }
      }
    }
  }

  /// Records the capture sequence each cell was delivered up to. A tile
  /// refresh snapshots the host's current pixels, so it counts as the newest
  /// frame the client has seen.
  private noteReceived(frame: Frame, refresh: boolean): void {
    const seq = refresh ? Math.max(0, this.latestFrameSeq) : frame.frameSeq
    for (const tile of frame.tiles) {
      for (const key of this.tileCellKeys(tile.x, tile.y, tile.w, tile.h)) {
        const previous = this.cellReceivedSeq.get(key)
        if (previous === undefined || seq > previous) {
          this.cellReceivedSeq.set(key, seq)
        }
      }
    }
  }

  /// Queue a tile-level refresh for a dropped frame. Cells already covered by
  /// a newer frame (greater capture timestamp) are skipped: the picture there
  /// is newer than the dropped packet.
  private queueTileRefresh(frame: Frame, tsUs: number): void {
    if (!this.hasImage) {
      this.requestKeyframeThrottled()
      return
    }
    for (const tile of frame.tiles) {
      const keys = this.tileCellKeys(tile.x, tile.y, tile.w, tile.h)
      const stale = keys.some((key) => {
        const applied = this.tileAppliedAt.get(key)
        return applied === undefined || applied < tsUs
      })
      if (!stale) continue
      const key = `${tile.x},${tile.y},${tile.w},${tile.h}`
      this.pendingTiles.set(key, {
        x: tile.x,
        y: tile.y,
        w: tile.w,
        h: tile.h,
        ts: tsUs,
      })
    }
    if (this.pendingTiles.size === 0 || this.refreshTimer !== null) return
    this.refreshTimer = window.setTimeout(() => {
      this.refreshTimer = null
      if (!this.isLive() || this.pendingTiles.size === 0) return
      // Re-check right before sending: a newer frame may have covered these
      // tiles while the mark was queued (a laggy link can deliver the mark
      // after the replacement pixels).
      const tiles = Array.from(this.pendingTiles.values())
        .filter((tile) =>
          this.tileCellKeys(tile.x, tile.y, tile.w, tile.h).some((key) => {
            const applied = this.tileAppliedAt.get(key)
            return applied === undefined || applied < tile.ts
          }),
        )
        .slice(0, 64)
      this.pendingTiles.clear()
      if (tiles.length === 0) return
      this.sendJson({
        type: 'request-tiles',
        tiles: tiles.map(({ x, y, w, h }) => ({ x, y, w, h })),
      })
    }, 200)
  }

  /// Any message proves the link is alive. A pong can sit behind a large frame
  /// on the same TCP relay stream, so the watchdog measures total silence
  /// rather than pong latency.
  private noteInbound(): void {
    this.pendingPingAt = 0
    this.missedPongs = 0
  }

  private startPing(): void {
    this.stopPing()
    const ping = () => {
      if (!this.isLive()) return
      if (this.pendingPingAt !== 0) {
        this.missedPongs += 1
        if (this.missedPongs >= kPingMissLimit) {
          this.recoverStalledSession('the link stopped responding')
          return
        }
      } else {
        this.missedPongs = 0
      }
      this.pendingPingAt = Date.now()
      this.sendJson({ type: 'ping', t: performance.now() })
    }
    ping()
    this.pingTimer = window.setInterval(ping, kPingIntervalMs)
  }

  /// Coalesces keyframe requests while stale frames are being dropped.
  private vpxDecoder(codec: VpxCodec): VpxStreamDecoder {
    let decoder = this.vpxDecoders[codec]
    if (decoder === undefined) {
      decoder = new VpxStreamDecoder(codec)
      this.vpxDecoders[codec] = decoder
    }
    return decoder
  }

  private requestKeyframeThrottled(): void {
    const now = performance.now()
    if (now - this.lastKeyframeRequest < 300) return
    this.lastKeyframeRequest = now
    this.requestKeyframe()
  }

  private stopPing(): void {
    if (this.pingTimer !== null) {
      window.clearInterval(this.pingTimer)
      this.pingTimer = null
    }
    this.pendingPingAt = 0
    this.missedPongs = 0
  }

  /// The session looks live but nothing is coming back. Drop the socket and
  /// reconnect; the resume token keeps this from spending another code use.
  private recoverStalledSession(reason: string): void {
    if (this.closedByUser || !this.isLive()) return
    // Retry the local proxy on every reconnection. It may have failed once
    // while the core was restarting (an update, a crash), and the fallback to
    // the host's own socket has no AV1 and no AV1 stream: the session would
    // stay on tiles until the tab was reopened.
    this.directProxyFailed = false
    const ws = this.ws
    this.ws = null
    this.stopPing()
    this.stopHelloRetry()
    this.pendingFrame = false
    this.missedFrameResponses = 0
    if (ws !== null) {
      try {
        ws.close(4000, reason)
      } catch {
        // The socket may already be unusable; reconnecting is what matters.
      }
    }
    this.handlers.onPhase?.('connecting')
    this.openSocket()
  }

  /// One whole MOLK packet on the active transport.
  private sendRawPacket(packet: ArrayBuffer | Uint8Array): void {
    if (this.wt !== null && this.wt.isOpen) {
      const bytes = packet instanceof Uint8Array ? packet : new Uint8Array(packet)
      // packet_type::file_data = 0x12: file chunks take the dedicated stream.
      if (bytes.length > 5 && bytes[5] === 0x12 && this.wt.sendFile(bytes)) {
        return
      }
      this.wt.sendBinary(bytes)
      return
    }
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return
    this.ws.send(packet as BufferSource)
  }

  /// Brings up a WebTransport session and moves the client onto it.
  ///
  /// The socket stays alive until the host answers the hello sent over the new
  /// session: if anything fails, the page simply keeps using the socket.
  private async startWebTransport(
    welcome: Welcome,
    host: string,
    token: string,
  ): Promise<void> {
    const info = welcome.webtransport
    if (this.wt !== null || this.wtPending || this.wtDisabled) return
    if (info === undefined) return
    if (!webTransportSupported()) return
    const hash = fingerprintBytes(info.sha256)
    if (hash === null) return
    const url = `https://${formatHost(host)}:${info.port}/wt`
    const transport = await WtTransport.connect(url, hash, {
      onText: (text) => this.handleText(text),
      onBinary: (data) =>
        this.handleBinary(
          data.buffer.slice(
            data.byteOffset,
            data.byteOffset + data.byteLength,
          ) as ArrayBuffer,
        ),
      onClose: () => this.fallBackToSocket('webtransport closed'),
      onError: () => this.fallBackToSocket('webtransport failed'),
    })
    if (transport === null) return
    this.wtPending = true
    this.wt = transport
    const hello: Record<string, unknown> = {
      type: 'hello',
      token,
      seq_clock: true,
      video_push: true,
      h264_stream: this.h264Capable,
      client: 'molink-web',
      protocol: 1,
    }
    if (this.resumeToken !== '') hello.session = this.resumeToken
    transport.sendText(JSON.stringify(hello))
    // The file stream is a second byte pipe with its own channel tag; a
    // transfer on it never queues behind control messages.
    void transport.openFileStream()
  }

  /// Moves the page back onto the socket after the WebTransport session died.
  ///
  /// WebTransport replaces the socket, so losing it used to end the session
  /// outright. The socket door is still there: reopen it with the resume
  /// token (no new code needed) and stay on it -- a flapping WebTransport
  /// endpoint must not trade the session back and forth.
  private fallBackToSocket(reason: string): void {
    if (this.closedByUser) return
    if (this.wt === null && !this.wtPending) return
    const transport = this.wt
    const wasPending = this.wtPending
    this.wt = null
    this.wtPending = false
    this.wtDisabled = true
    if (transport !== null) transport.close()

    // The WebTransport never took over: the socket is still open and live,
    // so falling back is just dropping the attempt.
    if (wasPending && this.ws !== null &&
        this.ws.readyState === WebSocket.OPEN) {
      return
    }
    if (this.lastConnect === null) {
      this.handlers.onPhase?.('disconnected', reason)
      return
    }
    this.handlers.onPhase?.('connecting', reason)
    this.retryAttempts = 0
    this.openSocket()
  }

  /// Drops the socket after the WebTransport session took over.
  private retireSocket(): void {
    const ws = this.ws
    this.ws = null
    if (ws !== null && ws.readyState <= WebSocket.OPEN) {
      ws.close(1000, 'switched to webtransport')
    }
  }

  private sendJson(payload: Record<string, unknown>, viaSocket = false): void {
    const text = JSON.stringify(payload)
    // Control prefers the WebRTC data channel once it is up: it rides the same
    // encrypted path as the video track and is not queued behind file bytes on
    // the relay socket. Signaling stays on the socket -- it is what brings the
    // channel up in the first place.
    if (!viaSocket && this.wt !== null && this.wt.isOpen) {
      this.wt.sendText(text)
      return
    }
    const channel = this.rtcChannel
    if (!viaSocket && channel !== null && channel.readyState === 'open') {
      try {
        channel.send(text)
        return
      } catch {
        // Fall through to the socket.
      }
    }
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return
    this.ws.send(text)
  }
}

