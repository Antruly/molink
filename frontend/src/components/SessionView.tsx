import {
  Activity,
  ChevronDown,
  ClipboardCheck,
  ClipboardCopy,
  ClipboardPaste,
  ClipboardX,
  ExternalLink,
  FolderOpen,
  Cpu,
  Gauge,
  GripHorizontal,
  ShieldCheck,
  ShieldOff,
  Hand,
  Keyboard,
  LogOut,
  Maximize2,
  MessageSquare,
  Mic,
  MicOff,
  Minimize2,
  Monitor,
  Moon,
  MousePointer2,
  Pin,
  PinOff,
  Phone,
  PhoneOff,
  Radio,
  Settings2,
  Sun,
  Volume2,
  VolumeX,
  Wrench,
  X,
} from 'lucide-react'
import { createPortal } from 'react-dom'
import {
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
  type ReactNode,
  type PointerEvent as ReactPointerEvent,
} from 'react'
import { useTranslation } from 'react-i18next'
import {
  FPS_CHOICES,
  MIN_FPS,
  fpsPlan,
  parseFpsSetting,
} from '../lib/frame-rate'
import { Codec, codecName } from '../lib/protocol'
import { collectDropEntries, stageDroppedFiles } from '../lib/file-drop'
import { shellInfo, windowAction } from '../lib/shell'
import { openSessionWindow } from '../lib/windows'
import {
  sessionConnection,
  useAppStore,
  type QualityMode,
  type SessionTab,
} from '../store/app'
import { RemoteCursor } from './RemoteCursor'
import { TileCanvas, type DisplayMode } from './TileCanvas'
import { ChatPanel } from './ChatPanel'
import { FileTransferPanel } from './FileTransferPanel'
import type { ChatMessage } from '../lib/client'
import { VoiceCall } from '../lib/voice'

function formatBandwidth(bytesPerSecond: number): string {
  if (bytesPerSecond >= 1024 * 1024) {
    return `${(bytesPerSecond / (1024 * 1024)).toFixed(1)} MB/s`
  }
  if (bytesPerSecond >= 1024) {
    return `${Math.round(bytesPerSecond / 1024)} KB/s`
  }
  return `${bytesPerSecond} B/s`
}

/// Keys typed into local UI (chat, settings, forms) must stay local. Panels
/// that contain local controls mark themselves with `data-local-input` so a
/// focused button inside them is covered too.
function isLocalInputTarget(target: EventTarget | null): boolean {
  const element = target as HTMLElement | null
  if (element === null || typeof element.closest !== 'function') return false
  const tag = element.tagName
  if (
    tag === 'INPUT' ||
    tag === 'TEXTAREA' ||
    tag === 'SELECT' ||
    element.isContentEditable
  ) {
    return true
  }
  return element.closest('[data-local-input]') !== null
}

type OrientationLockApi = {
  lock?: (orientation: string) => Promise<void>
  unlock?: () => void
}

/// Mobile fullscreen should be landscape. The API is best-effort: it needs an
/// active fullscreen element and is missing on iOS Safari, where the request
/// simply stays a no-op.
function lockLandscapeOrientation(): void {
  try {
    const orientation = window.screen
      ?.orientation as unknown as OrientationLockApi | undefined
    if (orientation && typeof orientation.lock === 'function') {
      void orientation.lock('landscape').catch(() => undefined)
    }
  } catch {
    // Orientation locking is optional.
  }
}

function unlockScreenOrientation(): void {
  try {
    const orientation = window.screen
      ?.orientation as unknown as OrientationLockApi | undefined
    if (orientation && typeof orientation.unlock === 'function') {
      orientation.unlock()
    }
  } catch {
    // Ignore.
  }
}

/// `KeyboardEvent.code` is reliable on physical keyboards but mobile virtual
/// keyboards often report `Unidentified`; fall back to the printable key.
function keyToCode(key: string): string {
  if (key.length === 1) {
    if (key >= 'a' && key <= 'z') return 'Key' + key.toUpperCase()
    if (key >= 'A' && key <= 'Z') return 'Key' + key
    if (key >= '0' && key <= '9') return 'Digit' + key
    const punctuation: Record<string, string> = {
      ' ': 'Space',
      '\n': 'Enter',
      '-': 'Minus',
      '=': 'Equal',
      '[': 'BracketLeft',
      ']': 'BracketRight',
      '\\': 'Backslash',
      ';': 'Semicolon',
      "'": 'Quote',
      '`': 'Backquote',
      ',': 'Comma',
      '.': 'Period',
      '/': 'Slash',
    }
    return punctuation[key] ?? ''
  }
  const named: Record<string, string> = {
    Enter: 'Enter',
    Backspace: 'Backspace',
    Tab: 'Tab',
    Escape: 'Escape',
    Delete: 'Delete',
    Home: 'Home',
    End: 'End',
    PageUp: 'PageUp',
    PageDown: 'PageDown',
    ArrowLeft: 'ArrowLeft',
    ArrowRight: 'ArrowRight',
    ArrowUp: 'ArrowUp',
    ArrowDown: 'ArrowDown',
  }
  return named[key] ?? ''
}

function formatCallTime(seconds: number): string {
  const minutes = Math.floor(seconds / 60)
  return `${minutes}:${String(seconds % 60).padStart(2, '0')}`
}

function touchPrimary(): boolean {
  try {
    if (new URLSearchParams(window.location.search).get('touch') === '1') {
      return true
    }
    return window.matchMedia?.('(pointer: coarse)').matches ?? false
  } catch {
    return false
  }
}


type ResolvedQuality = Exclude<QualityMode, 'auto'>

/// Capture rate and pull interval per preset. The pull interval is the real
/// ceiling: the client asks for the next frame only after the previous one
/// arrived, so a shorter interval is what makes "smooth" actually smoother.
/// Two independent axes, labelled as two chips: how the bytes travel (direct,
/// hole-punched peer-to-peer, or through a relay) and which wire protocol
/// carries them (WebSocket, QUIC, UDP, TCP...). A relayed QUIC session is
/// RELAY + QUIC, not a single mixed value.
const PATH_LABELS: Record<string, string> = {
  direct: 'DIRECT',
  p2p: 'P2P',
  relay: 'RELAY',
  webrtc: 'WEBRTC',
}

const PROTOCOL_LABELS: Record<string, string> = {
  websocket: 'WS',
  http2: 'HTTP/2',
  http3: 'HTTP/3',
  quic: 'QUIC',
  udp: 'UDP',
  tcp: 'TCP',
}

/// One compact toolbar button that opens a menu of related actions. The
/// session toolbar is long; when it would scroll, the button groups collapse
/// into these menus instead of squeezing every button into one row.
interface ToolbarMenuItem {
  key: string
  label: string
  icon?: ReactNode
  onSelect: () => void
  disabled?: boolean
  active?: boolean
}

function ToolbarMenu({
  icon,
  label,
  title,
  items,
}: {
  icon: ReactNode
  label: string
  title: string
  items: ToolbarMenuItem[]
}) {
  const [open, setOpen] = useState(false)
  const [spot, setSpot] = useState<{ left: number; top: number } | null>(null)
  const button = useRef<HTMLButtonElement>(null)
  const holder = useRef<HTMLDivElement>(null)
  const surface = useRef<HTMLDivElement>(null)
  useEffect(() => {
    if (!open) return
    const close = (event: Event) => {
      const target = event.target as Node
      // The popup is portalled to the body, so it is not inside `holder`.
      if (holder.current?.contains(target)) return
      if (surface.current?.contains(target)) return
      setOpen(false)
    }
    document.addEventListener('pointerdown', close, true)
    const onScroll = () => setOpen(false)
    window.addEventListener('resize', onScroll)
    return () => {
      document.removeEventListener('pointerdown', close, true)
      window.removeEventListener('resize', onScroll)
    }
  }, [open])
  // The toolbar scrolls horizontally (`overflow-x: auto`), which clips any
  // absolutely positioned child. The menu is therefore portalled to the body
  // and placed from the button's viewport rect.
  const toggle = () => {
    if (open) {
      setOpen(false)
      return
    }
    const rect = button.current?.getBoundingClientRect()
    if (!rect) return
    const width = 200
    setSpot({
      left: Math.max(8, Math.min(rect.left, window.innerWidth - width - 8)),
      top: rect.bottom + 6,
    })
    setOpen(true)
  }
  return (
    <div ref={holder} className="shrink-0">
      <button
        ref={button}
        type="button"
        onClick={toggle}
        className="btn btn-quiet flex h-7 shrink-0 items-center gap-0.5 !px-1.5"
        title={title}
        aria-label={title}
        aria-haspopup="menu"
        aria-expanded={open}
      >
        {icon}
        <ChevronDown size={10} />
      </button>
      {open && spot
        ? createPortal(
            <div
              ref={surface}
              role="menu"
              aria-label={label}
              data-toolbar-menu="true"
              style={{ position: 'fixed', left: spot.left, top: spot.top }}
              className="z-[100] min-w-44 rounded-md border border-line bg-surface p-1 text-[12px] text-ink shadow-lg"
            >
              {items.map((item) => (
                <button
                  key={item.key}
                  type="button"
                  role="menuitem"
                  disabled={item.disabled}
                  onClick={() => {
                    setOpen(false)
                    item.onSelect()
                  }}
                  className={
                    'flex w-full items-center gap-2 rounded px-2 py-1.5 text-left ' +
                    (item.active ? 'text-accent ' : '') +
                    (item.disabled ? 'opacity-40' : 'hover:bg-panel')
                  }
                >
                  {item.icon}
                  <span className="flex-1">{item.label}</span>
                </button>
              ))}
            </div>,
            document.body,
          )
        : null}
    </div>
  )
}

export function SessionView({
  tab,
  popout = false,
}: {
  tab: SessionTab
  popout?: boolean
}) {
  const { t } = useTranslation()
  const localMode = useAppStore((state) => state.localMode)
  const qualityWatermark = useAppStore((state) => state.qualityWatermark)
  const autoAudio = useAppStore((state) => state.autoAudio)
  const setQualityWatermark = useAppStore((state) => state.setQualityWatermark)
  const qualityMode = useAppStore((state) => state.qualityMode)
  const setQualityMode = useAppStore((state) => state.setQualityMode)
  const frameRate = useAppStore((state) => state.frameRate)
  const setFrameRate = useAppStore((state) => state.setFrameRate)
  const setControlEnabled = useAppStore((state) => state.setControlEnabled)
  const closeTab = useAppStore((state) => state.closeTab)
  const createSession = useAppStore((state) => state.createSession)
  const setImmersive = useAppStore((state) => state.setImmersive)
  const setWindowFullscreen = useAppStore((state) => state.setWindowFullscreen)
  const setClipboardEnabled = useAppStore((state) => state.setClipboardEnabled)
  const immersive = useAppStore((state) => state.immersive)
  const theme = useAppStore((state) => state.theme)
  const toggleTheme = useAppStore((state) => state.toggleTheme)
  const setSettingsOpen = useAppStore((state) => state.setSettingsOpen)

  const welcome = tab.welcome
  const synthetic = welcome?.tiles.synthetic === true
  const inputAvailable = welcome?.input === true && !synthetic
  const clipboardAvailable = welcome?.clipboard === true
  // The host walks the quality ladder itself when the preset is "auto".
  const hostAutoQuality = welcome?.auto_quality === true

  const surfaceRef = useRef<HTMLDivElement>(null)
  const pendingMove = useRef<{ x: number; y: number } | null>(null)
  const moveScheduled = useRef(false)
  const pressedKeys = useRef<Set<string>>(new Set())
  const pressedButtons = useRef<Set<number>>(new Set())
  const [surfaceSize, setSurfaceSize] = useState({ width: 0, height: 0 })
  const [actionError, setActionError] = useState<string | null>(null)
  const [retryCode, setRetryCode] = useState('')
  const touchDevice = touchPrimary()
  const [virtualMouse, setVirtualMouse] = useState(false)
  const [displayMode, setDisplayMode] = useState<DisplayMode>('fit')
  const [elementFullscreen, setElementFullscreen] = useState(
    () => document.fullscreenElement !== null,
  )
  const isFullscreen = elementFullscreen || immersive
  const [chatMessages, setChatMessages] = useState<ChatMessage[]>([])
  const [chatOpen, setChatOpen] = useState(false)
  const [chatUnread, setChatUnread] = useState(0)
  const chatOpenRef = useRef(false)
  const [filesOpen, setFilesOpen] = useState(false)
  // The player lives in the connection, so a background tab keeps playing and
  // the toggle reflects that when the tab is shown again.
  const [audioOn, setAudioOn] = useState(
    () => sessionConnection(tab.id)?.audioActive() ?? false,
  )
  const [audioVolume, setAudioVolume] = useState(
    () => sessionConnection(tab.id)?.getAudioVolume() ?? 0.8,
  )
  const audioAvailable = welcome?.audio === true && welcome.audio_format != null
  // A manual mute sticks for this connection; the preference only decides
  // what happens when a session comes up.
  const audioMutedRef = useRef(false)
  const audioVolumeRef = useRef(audioVolume)
  audioVolumeRef.current = audioVolume
  const autoAvailableRef = useRef(audioAvailable)
  autoAvailableRef.current = audioAvailable
  const [callPhase, setCallPhase] = useState<'idle' | 'ringing' | 'active'>(
    'idle',
  )
  const [callStartedAt, setCallStartedAt] = useState<number | null>(null)
  const [callMuted, setCallMuted] = useState(false)
  const [callNotice, setCallNotice] = useState<string | null>(null)
  const [hostMic, setHostMic] = useState(true)
  const [callSeconds, setCallSeconds] = useState(0)
  const voiceRef = useRef<VoiceCall | null>(null)
  const callAvailable = welcome?.call === true
  // The call bar floats over the remote picture; let the user drag it out of
  // the way (it used to sit on top of the friendly toolbar with no handle).
  const callBarRef = useRef<HTMLDivElement>(null)
  const callDrag = useRef<{
    offsetX: number
    offsetY: number
    startX: number
    startY: number
    moved: boolean
  } | null>(null)
  const [callPos, setCallPos] = useState<{ x: number; y: number } | null>(null)
  const [quickOpen, setQuickOpen] = useState(false)
  const [quickPos, setQuickPos] = useState<{ x: number; y: number } | null>(null)
  const quickBarRef = useRef<HTMLDivElement>(null)
  /// The toolbar would scroll at this width: collapse the button groups into
  /// menus instead of hiding actions behind a horizontal scroll.
  const [compactBar, setCompactBar] = useState(false)
  /// Pinned toolbar: stays open instead of collapsing back into the floating
  /// ball. The choice is a per-device UI preference, kept in localStorage.
  const [barPinned, setBarPinned] = useState(false)
  /// Width the expanded toolbar needs, measured while it still fits.
  const expandedBarWidth = useRef(0)
  const quickDrag = useRef<{
    offsetX: number
    offsetY: number
    startX: number
    startY: number
    moved: boolean
  } | null>(null)
  const quickSuppressClick = useRef(false)
  const [mobilePos, setMobilePos] = useState<{ x: number; y: number } | null>(
    null,
  )
  // The touch toolbar lives behind an edge badge instead of covering the
  // remote picture all the time.
  const [mobileToolsOpen, setMobileToolsOpen] = useState(false)
  const mobileDockRef = useRef<HTMLDivElement>(null)
  const mobileDrag = useRef<{
    offsetX: number
    offsetY: number
    startX: number
    startY: number
  } | null>(null)
  const [autoTier, setAutoTier] = useState<ResolvedQuality>('balanced')
  const sessionRootRef = useRef<HTMLDivElement>(null)
  const panPointer = useRef<{ x: number; y: number } | null>(null)
  const statsRef = useRef(tab.stats)
  const hostRef = useRef({ fps: 0, buildMs: 0 })
  const autoModeRef = useRef<QualityMode>('balanced')
  const autoRef = useRef<{
    tier: ResolvedQuality | null
    streak: number
    lastChange: number
  }>({ tier: null, streak: 0, lastChange: 0 })
  const keyboardRef = useRef<HTMLTextAreaElement>(null)
  const virtualPos = useRef({ x: 0.5, y: 0.5 })
  const touchPoints = useRef(new Map<number, { x: number; y: number }>())
  const touchStart = useRef<{
    time: number
    x: number
    y: number
    moved: boolean
  } | null>(null)
  const longPressTimer = useRef<number | null>(null)
  const suppressTap = useRef(false)
  const scrollAccum = useRef({ x: 0, y: 0 })
  const lastCentroid = useRef<{ x: number; y: number } | null>(null)
  const composingRef = useRef(false)

  const resolvedMode: ResolvedQuality =
    qualityMode === 'auto' ? autoTier : qualityMode
  // Stream codecs the toolbar may offer: the host can encode them and this
  // page can decode them. Empty for hosts that do not advertise the list.
  const codecOptions = tab.codecOptions ?? []
  const codecPreference = tab.codecPreference ?? 'auto'
  // Average frame rate: the user's choice (or the tier window in auto), then
  // lowered by the load controller while the host or the link is saturated.
  const [targetFps, setTargetFps] = useState(() =>
    fpsPlan(frameRate, resolvedMode).start,
  )
  const targetFpsRef = useRef(targetFps)
  const frameRateRef = useRef(frameRate)
  const qualityModeRef = useRef(qualityMode)
  const planRef = useRef(fpsPlan(frameRate, resolvedMode))
  const fpsState = useRef({
    target: targetFps,
    good: 0,
    bad: 0,
    lastChange: 0,
  })
  const pullMs = Math.max(16, Math.round(1000 / Math.max(1, targetFps)))

  const connection = useCallback(
    () => sessionConnection(tab.id),
    [tab.id],
  )

  useEffect(() => {
    if (tab.phase !== 'live') {
      audioMutedRef.current = false
      return
    }
    if (!autoAudio || !autoAvailableRef.current) return
    if (audioMutedRef.current || audioOn) return
    const conn = connection()
    if (!conn) return
    const start = () => {
      conn.setAudioVolume(audioVolumeRef.current)
      conn.startAudio()
      setAudioOn(conn.audioActive())
    }
    start()
    if (conn.audioActive()) return
    // The browser's autoplay policy can refuse an AudioContext that was not
    // created inside a gesture. The connect click happened a while ago, so
    // retry on the first interaction the user makes with the page instead of
    // making them find the speaker button.
    const kick = () => {
      if (audioMutedRef.current) return
      start()
    }
    window.addEventListener('pointerdown', kick, { once: true })
    window.addEventListener('keydown', kick, { once: true })
    return () => {
      window.removeEventListener('pointerdown', kick)
      window.removeEventListener('keydown', kick)
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [tab.phase, autoAudio, audioOn, connection])

  // The host has 60 s to approve an access request; show the remaining time
  // and let the requester cancel instead of waiting forever.
  const [approvalNow, setApprovalNow] = useState(0)
  const approvalLeft =
    tab.notice === 'waiting_approval' && tab.approvalDeadline && approvalNow > 0
      ? Math.max(0, Math.ceil((tab.approvalDeadline - approvalNow) / 1000))
      : null
  useEffect(() => {
    if (tab.notice !== 'waiting_approval') return
    const tick = () => setApprovalNow(Date.now())
    const immediate = window.setTimeout(tick, 0)
    const timer = window.setInterval(tick, 500)
    return () => {
      window.clearTimeout(immediate)
      window.clearInterval(timer)
    }
  }, [tab.notice])
  const cancelApproval = useCallback(() => {
    connection()?.disconnect()
    closeTab(tab.id)
  }, [connection, closeTab, tab.id])
  useEffect(() => {
    if (tab.notice !== 'waiting_approval' || approvalLeft !== 0) return
    cancelApproval()
  }, [approvalLeft, cancelApproval, tab.notice])
  // Countdown to the next automatic reconnect while the session is down.
  const [retryNow, setRetryNow] = useState(0)
  const retryLeft =
    tab.phase === 'disconnected' && tab.retryAt && retryNow > 0
      ? Math.max(0, Math.ceil((tab.retryAt - retryNow) / 1000))
      : null
  useEffect(() => {
    if (tab.phase !== 'disconnected') return
    const tick = () => setRetryNow(Date.now())
    const immediate = window.setTimeout(tick, 0)
    const timer = window.setInterval(tick, 500)
    return () => {
      window.clearTimeout(immediate)
      window.clearInterval(timer)
    }
  }, [tab.phase, tab.retryAt])

  const requestAccessFromError = useCallback(() => {
    createSession(tab.host, tab.port, '', tab.label, false, true, tab.mode, tab.intent, tab.deviceId)
    closeTab(tab.id)
  }, [closeTab, createSession, tab.host, tab.label, tab.port, tab.id, tab.mode, tab.intent, tab.deviceId])
  const reconnect = useCallback(() => {
    connection()?.connect(tab.host, tab.port, tab.token, tab.token === '', tab.deviceId)
  }, [connection, tab.host, tab.port, tab.token, tab.deviceId])
  // A wrong verification code is a policy answer, not a network failure.
  // Ask for a new code instead of reconnecting with the same one.
  const needsCode = tab.errorCode === 'unauthorized'
  // A rejected display-mode change is not a broken link: reconnecting cannot
  // fix it, and the backend cannot apply the mode at all.
  const displayModeError =
    tab.errorCode === 'display_mode_failed' ||
    tab.errorCode === 'display_mode_unsupported' ||
    tab.errorCode === 'display_mode_busy'
  const submitCode = () => {
    const value = retryCode.trim()
    if (value === '') return
    setRetryCode('')
    connection()?.connect(tab.host, tab.port, value, false, tab.deviceId)
  }

  // A background tab is throttled and may miss frames; resync as soon as it
  // becomes visible again instead of waiting for the next pull.
  useEffect(() => {
    const onVisible = () => {
      if (document.visibilityState !== 'visible') return
      if (tab.phase !== 'live') return
      // The per-session back buffer is kept current by the background pull, so
      // the returning view only needs the newest frame, not a keyframe.
      connection()?.requestFrame()
    }
    document.addEventListener('visibilitychange', onVisible)
    return () => document.removeEventListener('visibilitychange', onVisible)
  }, [connection, tab.phase])

  useEffect(() => {
    const points = touchPoints.current
    return () => {
      if (longPressTimer.current !== null) {
        window.clearTimeout(longPressTimer.current)
        longPressTimer.current = null
      }
      points.clear()
      touchStart.current = null
      suppressTap.current = false
    }
  }, [tab.id])

  useEffect(() => {
    if (tab.phase !== 'live') return
    const timer = window.setInterval(() => {
      if (document.visibilityState === 'hidden') return
      connection()?.requestFrame()
    }, pullMs)
    // WebKitGTK marks a window hidden when it loses focus. Keep a slow pull so
    // the session still receives its first frame and stays current instead of
    // freezing until the user clicks the window.
    const hiddenTimer = window.setInterval(() => {
      if (document.visibilityState !== 'hidden') return
      connection()?.requestFrame()
    }, 1000)
    return () => {
      window.clearInterval(timer)
      window.clearInterval(hiddenTimer)
    }
  }, [tab.phase, tab.id, connection, pullMs])

  useEffect(() => {
    const update = () => {
      const active = document.fullscreenElement !== null
      setElementFullscreen(active)
      // In a browser the Fullscreen API is the only fullscreen surface; Esc
      // must also bring the app chrome back.
      if (!active && !shellInfo.enabled) setImmersive(false)
    }
    document.addEventListener('fullscreenchange', update)
    return () => document.removeEventListener('fullscreenchange', update)
  }, [setImmersive])

  useEffect(() => {
    chatOpenRef.current = chatOpen
  }, [chatOpen])

  function stopVoiceCall() {
    voiceRef.current?.stop()
    voiceRef.current = null
    setCallPhase('idle')
    setCallStartedAt(null)
    setCallMuted(false)
    setCallSeconds(0)
  }

  function handleCallEvent(event: {
    action: string
    hostMicrophone?: boolean
    startedMs?: number
    message?: string
  }) {
    if (event.action === 'ringing') {
      setCallPhase('ringing')
      setCallNotice(null)
    } else if (event.action === 'accepted') {
      setCallPhase('active')
      setCallStartedAt(event.startedMs ?? Date.now())
      setCallSeconds(0)
      setCallNotice(null)
      setHostMic(event.hostMicrophone !== false)
      const voice = new VoiceCall()
      voiceRef.current = voice
      const activeConnection = connection()
      void voice
        .start((pcm) => activeConnection?.sendVoice(pcm))
        .then((result) => {
          if (result.mic) return
          setCallNotice(
            result.reason === 'unsupported'
              ? 'mic_insecure'
              : result.reason === 'denied'
                ? 'mic_denied'
                : result.reason === 'missing'
                  ? 'mic_missing'
                  : result.reason === 'busy'
                    ? 'mic_busy'
                    : 'no_mic',
          )
        })
    } else if (event.action === 'declined') {
      stopVoiceCall()
      setCallNotice('declined')
    } else if (event.action === 'ended') {
      stopVoiceCall()
    } else if (event.action === 'audio_error') {
      setCallNotice('audio_error')
    }
  }

  const startVoiceCall = () => {
    connection()?.inviteCall()
    setCallPhase('ringing')
    setCallNotice(null)
  }

  const hangupVoiceCall = () => {
    connection()?.hangupCall()
    stopVoiceCall()
  }

  const toggleCallMute = () => {
    const next = !callMuted
    setCallMuted(next)
    voiceRef.current?.setMuted(next)
  }

  // A mounted view is the foreground session: full-rate pull, and the only
  // one allowed to write the local text clipboard from the remote.
  useEffect(() => {
    const active = connection()
    if (!active) return
    active.setForeground(true)
    return () => active.setForeground(false)
  }, [connection])

  useEffect(() => {
    const active = connection()
    if (!active) return
    active.requestChatHistory()
    return active.addListener((event) => {
      if (event.kind === 'chat-history') {
        setChatMessages(event.messages)
      } else if (event.kind === 'chat') {
        setChatMessages((previous) =>
          [...previous, event.message].slice(-200),
        )
        if (!chatOpenRef.current) setChatUnread((count) => count + 1)
      } else if (event.kind === 'voice') {
        voiceRef.current?.pushRemote(event.data)
      } else if (event.kind === 'call') {
        handleCallEvent(event)
      }
    })
  }, [connection])

  // The automatic file clipboard sync lives in the connection so background
  // tabs (and popouts) keep mirroring local files too; only the files panel
  // needs to pause it while it uploads through the same ids.
  useEffect(() => {
    connection()?.setFilePanelOpen(filesOpen)
  }, [connection, filesOpen])

  useEffect(() => {
    if (callPhase !== 'active' || callStartedAt === null) return
    const timer = window.setInterval(() => {
      setCallSeconds(Math.floor((Date.now() - callStartedAt) / 1000))
    }, 1000)
    return () => window.clearInterval(timer)
  }, [callPhase, callStartedAt])

  useEffect(() => {
    return () => {
      voiceRef.current?.stop()
      voiceRef.current = null
      sessionConnection(tab.id)?.hangupCall()
    }
  }, [tab.id])

  // A files/chat connection from the simple home opens that panel as soon as
  // the session is live; the desktop view stays available behind it.
  useEffect(() => {
    if (tab.phase !== 'live') return
    if (tab.intent !== 'files' && tab.intent !== 'chat') return
    const open = tab.intent
    const timer = window.setTimeout(() => {
      if (open === 'files') {
        setFilesOpen(true)
        setChatOpen(false)
      } else {
        setChatOpen(true)
        setFilesOpen(false)
      }
    }, 0)
    return () => window.clearTimeout(timer)
  }, [tab.phase, tab.id, tab.intent])


  useEffect(() => {
    try {
      setBarPinned(window.localStorage.getItem('molink.toolbarPinned') === '1')
    } catch {
      // A blocked storage API just means the pin starts off.
    }
  }, [])

  useEffect(() => {
    if (barPinned) setQuickOpen(true)
  }, [barPinned])

  // Collapse the toolbar into menus when its content would not fit. Expand
  // again only once there is room for the width it needed while expanded, so
  // the two states do not flip back and forth at the boundary.
  useEffect(() => {
    if (!quickOpen) return
    const bar = quickBarRef.current
    if (!bar) return
    const measure = () => {
      if (!compactBar) {
        const needed = bar.scrollWidth
        if (needed > 0) expandedBarWidth.current = needed
        if (needed > bar.clientWidth + 2) setCompactBar(true)
        return
      }
      if (expandedBarWidth.current > 0 &&
          bar.clientWidth >= expandedBarWidth.current) {
        setCompactBar(false)
      }
    }
    const observer = new ResizeObserver(measure)
    observer.observe(bar)
    measure()
    return () => observer.disconnect()
  }, [compactBar, quickOpen])

  // While the transfer drawer is open the whole window accepts a file drop:
  // the WebView's own drag destination only reaches the page when the page says
  // it wants the drop, and a drop that lands a few pixels off the drawer zone
  // used to be swallowed by the shell.
  useEffect(() => {
    if (!filesOpen) return
    const onDragOver = (event: globalThis.DragEvent) => {
      if (Array.from(event.dataTransfer?.types ?? []).includes('Files')) {
        event.preventDefault()
        if (event.dataTransfer) event.dataTransfer.dropEffect = 'copy'
      }
    }
    const onDrop = (event: globalThis.DragEvent) => {
      if (!event.dataTransfer) return
      event.preventDefault()
      void collectDropEntries(event.dataTransfer).then((entries) =>
        stageDroppedFiles(entries),
      )
    }
    document.addEventListener('dragover', onDragOver)
    document.addEventListener('drop', onDrop)
    return () => {
      document.removeEventListener('dragover', onDragOver)
      document.removeEventListener('drop', onDrop)
    }
  }, [filesOpen])

  // The expanded quick bar collapses when focus or the pointer moves outside.
  useEffect(() => {
    if (!quickOpen || barPinned) return
    const onPointerDown = (event: PointerEvent) => {
      const target = event.target as Element | null
      // A menu is portalled to the body: it is outside the bar's DOM but still
      // part of the toolbar, and closing here would unmount it mid-click.
      if (target?.closest?.('[data-toolbar-menu]')) return
      if (!quickBarRef.current?.contains(target as Node | null)) {
        setQuickOpen(false)
      }
    }
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') setQuickOpen(false)
    }
    document.addEventListener('pointerdown', onPointerDown, true)
    document.addEventListener('keydown', onKeyDown)
    return () => {
      document.removeEventListener('pointerdown', onPointerDown, true)
      document.removeEventListener('keydown', onKeyDown)
    }
  }, [quickOpen, barPinned])

  // In 1:1 mode the image can be larger than the window. Keep scrolling while
  // the pointer rests near an edge, so panning follows the mouse.
  useEffect(() => {
    if (displayMode !== 'actual') {
      panPointer.current = null
      return
    }
    const timer = window.setInterval(() => {
      const element = surfaceRef.current
      const point = panPointer.current
      if (!element || !point || virtualMouse) return
      const rect = element.getBoundingClientRect()
      const margin = 48
      const step = 16
      let dx = 0
      let dy = 0
      if (point.x < rect.left + margin) dx = -step
      else if (point.x > rect.right - margin) dx = step
      if (point.y < rect.top + margin) dy = -step
      else if (point.y > rect.bottom - margin) dy = step
      if (dx !== 0 || dy !== 0) element.scrollBy(dx, dy)
    }, 16)
    return () => window.clearInterval(timer)
  }, [displayMode, virtualMouse])

  // Keep the controllers' view of the link fresh without restarting their
  // intervals on every stats patch.
  useEffect(() => {
    statsRef.current = tab.stats
  }, [tab.stats])

  useEffect(() => {
    hostRef.current = {
      fps: tab.hostFps ?? 0,
      buildMs: tab.hostBuildMs ?? 0,
    }
  }, [tab.hostFps, tab.hostBuildMs])

  useEffect(() => {
    targetFpsRef.current = targetFps
  }, [targetFps])

  // Keep the plan and the adapted target in step. A user choice (rate or
  // quality) starts at its own resting value; an automatic quality-tier change
  // only clamps the current target into the new window, so a downgrade does
  // not immediately raise the rate again.
  useEffect(() => {
    const plan = fpsPlan(frameRate, resolvedMode)
    planRef.current = plan
    const userChoice =
      frameRateRef.current !== frameRate ||
      qualityModeRef.current !== qualityMode
    frameRateRef.current = frameRate
    qualityModeRef.current = qualityMode
    let next: number
    if (userChoice) {
      next = plan.start
      fpsState.current = {
        target: next,
        good: 0,
        bad: 0,
        lastChange: performance.now(),
      }
    } else {
      next = Math.min(plan.max, Math.max(plan.min, fpsState.current.target))
      fpsState.current.target = next
      fpsState.current.good = 0
      fpsState.current.bad = 0
    }
    setTargetFps((current) => (current === next ? current : next))
  }, [frameRate, qualityMode, resolvedMode])

  // The preset is host-global: one capture serves every viewer. When another
  // viewer picks one, follow it — otherwise this tab's next push asserts its
  // own choice right back and the host re-encodes for whoever spoke last.
  //
  // Only a *new* notice is adopted. The remembered notice stays in the tab
  // state, and without this guard every local change was compared with that
  // stale value and immediately reverted, which left the preset pinned to
  // whatever the last broadcast said.
  const lastSharedQuality = useRef<string | null>(null)
  useEffect(() => {
    const shared = tab.sharedQuality?.mode
    if (!shared || shared === 'auto') return
    if (lastSharedQuality.current === shared) return
    lastSharedQuality.current = shared
    if (shared !== qualityMode) setQualityMode(shared as QualityMode)
  }, [tab.sharedQuality, qualityMode, setQualityMode])

  // Push the active preset and the average rate after a short settle delay.
  // Both are host-global, like the old capture-fps message.
  const lastPushedQuality = useRef<string | null>(null)
  useEffect(() => {
    if (tab.phase !== 'live') {
      lastPushedQuality.current = null
      return
    }
    const timer = window.setTimeout(() => {
      const conn = connection()
      if (!conn) return
      conn.setQuality(
        hostAutoQuality && qualityMode === 'auto' ? 'auto' : resolvedMode,
        targetFps,
      )
      // A preset change can change the tile format (turbo is gray8); without
      // a full frame the host only repaints tiles that happen to change, so
      // the rest of the picture stays in the previous format.
      const key = `${tab.id}:${resolvedMode}`
      if (lastPushedQuality.current !== key) {
        lastPushedQuality.current = key
        conn.requestKeyframe()
      }
    }, 400)
    return () => window.clearTimeout(timer)
  }, [tab.phase, tab.id, connection, resolvedMode, targetFps, hostAutoQuality, qualityMode])

  // Auto quality: the link only picks the image tier now; the frame rate has
  // its own controller below. A tier is lowered only once the rate is already
  // at the tier's floor, and raised when the link is healthy with headroom.
  useEffect(() => {
    if (tab.phase !== 'live' || qualityMode !== 'auto' || hostAutoQuality) {
      // Remember the preset we left so re-entering auto starts fresh.
      autoModeRef.current = qualityMode
      return
    }
    if (autoModeRef.current !== qualityMode) {
      autoModeRef.current = qualityMode
      autoRef.current = { tier: null, streak: 0, lastChange: 0 }
    }
    const timer = window.setInterval(() => {
      // A hidden window does not pull frames, so it must not drive the
      // host-global preset from a stale picture of the link.
      if (document.visibilityState === 'hidden') return
      const stats = statsRef.current
      const latency = stats.latencyMs
      const wireFps = stats.wireFps
      const bandwidth = stats.bytesPerSec
      // Compare the completed pull rate with the rate we asked for, so a
      // deliberately low target is not mistaken for a struggling link.
      const target = targetFpsRef.current
      const floor = planRef.current.floor
      const underrunning =
        wireFps > 0 && wireFps < Math.max(3, target * 0.5)
      const bad =
        (latency !== null && latency > 300) ||
        underrunning ||
        stats.dropped > 5
      const good =
        latency !== null &&
        latency < 90 &&
        stats.dropped === 0 &&
        wireFps >= Math.min(12, target)

      // Auto never picks the grayscale turbo preset: flipping the whole
      // picture between monochrome and colour every few seconds is far more
      // jarring than a lower frame rate. Turbo stays a manual choice; auto
      // moves between the colour tiers only.
      const order: ResolvedQuality[] = ['smooth', 'balanced', 'quality']
      const index = order.indexOf(resolvedMode)
      let tier = resolvedMode
      if (bad && target <= floor && index > 0) {
        tier = order[index - 1]!
      } else if (
        good &&
        index >= 0 &&
        index < order.length - 1 &&
        bandwidth < 12 * 1024 * 1024
      ) {
        tier = order[index + 1]!
      }

      const state = autoRef.current
      if (tier === resolvedMode) {
        state.tier = null
        state.streak = 0
        return
      }
      if (state.tier === tier) {
        state.streak += 1
      } else {
        state.tier = tier
        state.streak = 1
      }
      // Slow, asymmetric hysteresis: three bad samples to step down, five
      // good ones to step back up, with a long dwell so a marginal link
      // cannot make the picture change tiers every few seconds.
      const now = performance.now()
      const downgrade = order.indexOf(tier) < order.indexOf(resolvedMode)
      const needed = downgrade ? 3 : 5
      const dwell = downgrade ? 6000 : 10000
      if (state.streak >= needed && now - state.lastChange >= dwell) {
        state.lastChange = now
        state.tier = null
        state.streak = 0
        setAutoTier(tier)
      }
    }, 1000)
    return () => window.clearInterval(timer)
  }, [tab.phase, tab.id, qualityMode, resolvedMode])

  // Average frame rate. The configured rate is the goal; the controller only
  // leaves it when the host cannot encode fast enough (its reported build
  // time) or the link cannot carry it (latency, drops, unanswered pulls).
  // Reduction is quick and recovery is slow, so a burst cannot make the rate
  // oscillate.
  useEffect(() => {
    if (tab.phase !== 'live') return
    const timer = window.setInterval(() => {
      if (document.visibilityState === 'hidden') return
      const plan = planRef.current
      const state = fpsState.current
      const target = state.target
      const stats = statsRef.current
      const host = hostRef.current
      const intervalMs = 1000 / Math.max(1, target)
      // 85 % of the frame budget; the rest covers grabbing and sending.
      const cpuCeiling =
        host.buildMs > 0
          ? Math.max(MIN_FPS, Math.floor(850 / host.buildMs))
          : Number.POSITIVE_INFINITY
      const latencyBad = stats.latencyMs !== null && stats.latencyMs > 220
      const dropBad = stats.dropped > 2
      const underrun =
        stats.wireFps > 0 && stats.wireFps < Math.max(3, target * 0.6)
      const cpuBad = cpuCeiling < target
      const bad = latencyBad || dropBad || underrun || cpuBad
      // Matching the ceiling used above, so a rate that exactly fits the
      // encoder counts as healthy instead of oscillating around it.
      const good =
        !bad &&
        (stats.latencyMs === null || stats.latencyMs < 110) &&
        (host.buildMs <= 0 || host.buildMs <= 0.85 * intervalMs)
      const now = performance.now()
      if (bad) {
        state.good = 0
        state.bad += 1
        if (state.bad < 2 || now - state.lastChange < 1500) return
        let next = Math.max(plan.min, Math.floor(target * 0.75))
        // A link that only answers a fraction of the pulls has already told
        // us its ceiling; jumping straight there avoids a long staircase.
        if (underrun && stats.wireFps > 0) {
          next = Math.min(next, Math.max(plan.min, Math.floor(stats.wireFps)))
        }
        if (cpuBad) next = Math.min(next, cpuCeiling)
        next = Math.max(plan.min, Math.min(target - 1, next))
        if (next >= target) return
        state.target = next
        state.bad = 0
        state.lastChange = now
        setTargetFps(next)
      } else if (good) {
        state.bad = 0
        state.good += 1
        if (
          state.good < 4 ||
          target >= plan.max ||
          now - state.lastChange < 3000
        ) {
          return
        }
        // Never recover past what the encoder can actually sustain.
        const next = Math.min(
          plan.max,
          target + (target < 20 ? 5 : 10),
          cpuCeiling,
        )
        if (next <= target) return
        state.target = next
        state.good = 0
        state.lastChange = now
        setTargetFps(next)
      } else {
        state.good = 0
        state.bad = 0
      }
    }, 1000)
    return () => window.clearInterval(timer)
  }, [tab.phase, tab.id])

  // The remote cursor shape becomes the local pointer over the picture, so a
  // resize handle, a text caret, a link hand or the busy spinner looks the
  // same as on the host and the user can tell which control is under it. The
  // style sits on the surface element, so it only applies inside the remote
  // image; browsers cap cursor images at 128x128, and anything larger falls
  // back to the default arrow.
  const remoteCursorCss = useMemo(() => {
    const shape = tab.cursorShape
    if (!shape || shape.w <= 0 || shape.h <= 0) return undefined
    // Fall back to the local arrow when the remote pointer is hidden (a
    // fullscreen video hides it) or its bitmap paints nothing: mirroring a
    // blank cursor made the local pointer disappear entirely, and a frozen
    // session could leave it that way with nothing to click with.
    if (tab.cursorPosition && tab.cursorPosition.visible === false) {
      return undefined
    }
    let paints = false
    for (let i = 3; i < shape.rgba.length; i += 4) {
      if (shape.rgba[i] !== 0) {
        paints = true
        break
      }
    }
    if (!paints) return undefined
    // The host draws its cursor at native pixel size. When the picture is
    // scaled to fit the window, a native-size cursor looks oversized, so scale
    // it with the picture; 'actual' keeps 1:1 and browsers cap cursors at
    // 128x128, so anything larger falls back to the default arrow.
    const screenW = tab.frame?.screenW ?? welcome?.tiles.screen_w ?? 0
    const screenH = tab.frame?.screenH ?? welcome?.tiles.screen_h ?? 0
    let scaleX = 1
    let scaleY = 1
    if (displayMode === 'fill' && screenW > 0 && screenH > 0 &&
        surfaceSize.width > 0 && surfaceSize.height > 0) {
      scaleX = surfaceSize.width / screenW
      scaleY = surfaceSize.height / screenH
    } else if (screenW > 0 && screenH > 0 && surfaceSize.width > 0 &&
               surfaceSize.height > 0) {
      scaleX = scaleY = Math.min(surfaceSize.width / screenW,
                                 surfaceSize.height / screenH)
    }
    const width = Math.max(8, Math.round(shape.w * scaleX))
    const height = Math.max(8, Math.round(shape.h * scaleY))
    if (width > 128 || height > 128) return undefined

    const native = document.createElement('canvas')
    native.width = shape.w
    native.height = shape.h
    const nativeContext = native.getContext('2d')
    if (!nativeContext) return undefined
    const image = nativeContext.createImageData(shape.w, shape.h)
    image.data.set(shape.rgba)
    nativeContext.putImageData(image, 0, 0)

    let source = native
    if (width !== shape.w || height !== shape.h) {
      const scaled = document.createElement('canvas')
      scaled.width = width
      scaled.height = height
      const scaledContext = scaled.getContext('2d')
      if (!scaledContext) return undefined
      scaledContext.imageSmoothingEnabled = true
      scaledContext.drawImage(native, 0, 0, width, height)
      source = scaled
    }
    const hotspotX = Math.round(shape.hotspotX * (width / shape.w))
    const hotspotY = Math.round(shape.hotspotY * (height / shape.h))
    return `url("${source.toDataURL()}") ${hotspotX} ${hotspotY}, default`
  }, [tab.cursorShape, tab.cursorPosition?.visible, tab.frame?.screenW,
      tab.frame?.screenH, welcome, displayMode, surfaceSize.width,
      surfaceSize.height])

  useEffect(() => {
    const element = surfaceRef.current
    if (!element) return
    const update = () =>
      setSurfaceSize({
        width: element.clientWidth,
        height: element.clientHeight,
      })
    update()
    const observer = new ResizeObserver(update)
    observer.observe(element)
    return () => observer.disconnect()
  }, [])

  const normalize = (clientX: number, clientY: number) => {
    const element = surfaceRef.current
    if (!element) return null
    // TileCanvas sizes the canvas element to exactly the painted image (fit),
    // the stretched area (fill) or its intrinsic size (actual), so its rect is
    // the one rectangle that is correct in every mode. Scroll offsets in
    // actual mode are already included in getBoundingClientRect().
    const canvas = element.querySelector('canvas')
    const rect = canvas?.getBoundingClientRect()
    if (!rect || rect.width <= 0 || rect.height <= 0) return null
    const nx = (clientX - rect.left) / rect.width
    const ny = (clientY - rect.top) / rect.height
    if (nx < 0 || nx > 1 || ny < 0 || ny > 1) return null
    return { x: Number(nx.toFixed(5)), y: Number(ny.toFixed(5)) }
  }

  const sendVirtualMove = () => {
    const point = virtualPos.current
    connection()?.sendInput({
      k: 'move',
      x: Number(point.x.toFixed(5)),
      y: Number(point.y.toFixed(5)),
    })
  }

  const sendVirtualClick = (button: number) => {
    sendVirtualMove()
    connection()?.sendInput({ k: 'button', button, down: true })
    connection()?.sendInput({ k: 'button', button, down: false })
  }

  const cancelLongPress = () => {
    if (longPressTimer.current !== null) {
      window.clearTimeout(longPressTimer.current)
      longPressTimer.current = null
    }
  }

  const touchCentroid = () => {
    let x = 0
    let y = 0
    for (const point of touchPoints.current.values()) {
      x += point.x
      y += point.y
    }
    const count = Math.max(1, touchPoints.current.size)
    return { x: x / count, y: y / count }
  }

  const toggleVirtualMouse = () => {
    if (!tab.controlEnabled) setControlEnabled(tab.id, true)
    if (!virtualMouse) {
      const screenW = tab.frame?.screenW || welcome?.tiles.screen_w || 0
      const screenH = tab.frame?.screenH || welcome?.tiles.screen_h || 0
      const cursor = tab.cursorPosition
      if (cursor && cursor.visible && screenW > 0 && screenH > 0) {
        virtualPos.current = {
          x: Math.min(1, Math.max(0, cursor.x / screenW)),
          y: Math.min(1, Math.max(0, cursor.y / screenH)),
        }
      } else {
        virtualPos.current = { x: 0.5, y: 0.5 }
      }
    }
    setVirtualMouse((value) => !value)
  }

  const openKeyboard = () => {
    if (!tab.controlEnabled) setControlEnabled(tab.id, true)
    keyboardRef.current?.focus()
  }

  const toggleAudio = () => {
    const conn = connection()
    if (!conn) return
    if (audioOn) {
      audioMutedRef.current = true
      conn.stopAudio()
      setAudioOn(false)
      return
    }
    if (!welcome?.audio_format) return
    audioMutedRef.current = false
    conn.setAudioVolume(audioVolume)
    conn.startAudio()
    setAudioOn(conn.audioActive())
  }

  const changeAudioVolume = (value: number) => {
    setAudioVolume(value)
    connection()?.setAudioVolume(value)
  }

  const toggleFullscreen = useCallback(() => {
    const next = !isFullscreen
    if (next) {
      // Session fullscreen is the remote picture alone. The native window is
      // also fullscreen so no OS border or taskbar is visible; the in-page
      // chrome (toolbar, tab bar) is hidden by `immersive`. On touch devices
      // the toolbar collapses to its edge badge.
      setMobileToolsOpen(false)
      setImmersive(true)
      if (shellInfo.enabled) setWindowFullscreen(true)
      else
        void sessionRootRef.current
          ?.requestFullscreen()
          .catch(() => undefined)
    } else {
      setImmersive(false)
      if (shellInfo.enabled) setWindowFullscreen(false)
      else if (document.fullscreenElement) void document.exitFullscreen()
    }
    // The window manager resizes the WebView asynchronously. Re-measure once
    // more after the allocation has settled so the canvas and cursor overlay
    // use the new surface size immediately.
    window.setTimeout(() => {
      window.dispatchEvent(new Event('resize'))
      const element = surfaceRef.current
      if (element) {
        setSurfaceSize({
          width: element.clientWidth,
          height: element.clientHeight,
        })
      }
    }, 200)
  }, [isFullscreen, setImmersive, setWindowFullscreen])

  // Esc leaves immersive mode, unless input control is on: then Esc belongs
  // to the remote application.
  useEffect(() => {
    // While the floating toolbar is open Esc dismisses it first, exactly like
    // the quick bar outside fullscreen.
    if (!isFullscreen || quickOpen || tab.controlEnabled) return
    const onKey = (event: KeyboardEvent) => {
      if (event.key !== 'Escape') return
      event.preventDefault()
      toggleFullscreen()
    }
    window.addEventListener('keydown', onKey, true)
    return () => window.removeEventListener('keydown', onKey, true)
  }, [isFullscreen, quickOpen, tab.controlEnabled, toggleFullscreen])

  // Mobile fullscreen means landscape. The lock has to wait for the
  // fullscreen transition; iOS Safari does not implement it and the request
  // stays a no-op there.
  useEffect(() => {
    if (!isFullscreen) {
      unlockScreenOrientation()
      return
    }
    if (!touchDevice) return
    const timer = window.setTimeout(lockLandscapeOrientation, 250)
    return () => window.clearTimeout(timer)
  }, [isFullscreen, touchDevice])

  useEffect(() => () => unlockScreenOrientation(), [])

  // Leaving the session (tab closed, disconnect) must not leave the window in
  // fullscreen or the chrome hidden.
  useEffect(() => {
    return () => {
      if (useAppStore.getState().immersive) {
        setImmersive(false)
        setWindowFullscreen(false)
      }
    }
  }, [setImmersive, setWindowFullscreen])

  const changeDisplayMode = (mode: DisplayMode) => {
    setDisplayMode(mode)
    if (!barPinned) setQuickOpen(false)
  }

  const quickHandleDown = (event: ReactPointerEvent<HTMLButtonElement>) => {
    const surface = surfaceRef.current
    if (!surface) return
    event.preventDefault()
    event.stopPropagation()
    const rect = surface.getBoundingClientRect()
    const position = quickPos ?? {
      x: Math.max(4, rect.width / 2 - 18),
      y: 12,
    }
    quickDrag.current = {
      offsetX: event.clientX - rect.left - position.x,
      offsetY: event.clientY - rect.top - position.y,
      startX: event.clientX,
      startY: event.clientY,
      moved: false,
    }
    setQuickPos(position)
    event.currentTarget.setPointerCapture(event.pointerId)
  }

  const quickHandleMove = (event: ReactPointerEvent<HTMLButtonElement>) => {
    const drag = quickDrag.current
    const surface = surfaceRef.current
    if (!drag || !surface) return
    const rect = surface.getBoundingClientRect()
    if (
      Math.hypot(event.clientX - drag.startX, event.clientY - drag.startY) > 5
    ) {
      drag.moved = true
    }
    setQuickPos({
      x: Math.min(
        Math.max(event.clientX - rect.left - drag.offsetX, 4),
        Math.max(4, rect.width - 40),
      ),
      y: Math.min(
        Math.max(event.clientY - rect.top - drag.offsetY, 4),
        Math.max(4, rect.height - 40),
      ),
    })
  }

  const quickHandleUp = () => {
    const drag = quickDrag.current
    quickDrag.current = null
    if (!drag?.moved) return
    quickSuppressClick.current = true
    window.setTimeout(() => {
      quickSuppressClick.current = false
    }, 0)
  }


  const callHandleDown = (event: ReactPointerEvent<HTMLButtonElement>) => {
    const surface = surfaceRef.current
    const bar = callBarRef.current
    if (!surface || !bar) return
    event.preventDefault()
    event.stopPropagation()
    const rect = surface.getBoundingClientRect()
    const barRect = bar.getBoundingClientRect()
    const position = callPos ?? {
      x: barRect.left - rect.left,
      y: barRect.top - rect.top,
    }
    callDrag.current = {
      offsetX: event.clientX - rect.left - position.x,
      offsetY: event.clientY - rect.top - position.y,
      startX: event.clientX,
      startY: event.clientY,
      moved: false,
    }
    setCallPos(position)
    event.currentTarget.setPointerCapture(event.pointerId)
  }

  const callHandleMove = (event: ReactPointerEvent<HTMLButtonElement>) => {
    const drag = callDrag.current
    const surface = surfaceRef.current
    const bar = callBarRef.current
    if (!drag || !surface || !bar) return
    if (
      !drag.moved &&
      Math.hypot(event.clientX - drag.startX, event.clientY - drag.startY) > 4
    ) {
      drag.moved = true
    }
    const rect = surface.getBoundingClientRect()
    const maxX = Math.max(4, rect.width - bar.offsetWidth - 4)
    const maxY = Math.max(4, rect.height - bar.offsetHeight - 4)
    setCallPos({
      x: Math.min(maxX, Math.max(4, event.clientX - rect.left - drag.offsetX)),
      y: Math.min(maxY, Math.max(4, event.clientY - rect.top - drag.offsetY)),
    })
  }

  const callHandleUp = () => {
    callDrag.current = null
  }

  const toggleQuickOpen = () => {
    if (quickSuppressClick.current) {
      quickSuppressClick.current = false
      return
    }
    setQuickOpen((value) => !value)
  }

  const quickHandlePos = {
    x: Math.min(
      quickPos?.x ?? Math.max(4, surfaceSize.width / 2 - 18),
      Math.max(4, surfaceSize.width - 40),
    ),
    y: Math.min(
      quickPos?.y ?? 12,
      Math.max(4, surfaceSize.height - 40),
    ),
  }

  const callBarPos = {
    x: Math.min(callPos?.x ?? 12, Math.max(4, surfaceSize.width - 280)),
    y: Math.min(callPos?.y ?? 56, Math.max(4, surfaceSize.height - 40)),
  }

  const mobileDockDown = (event: ReactPointerEvent<HTMLButtonElement>) => {
    const surface = surfaceRef.current
    const dock = mobileDockRef.current
    if (!surface || !dock) return
    event.preventDefault()
    event.stopPropagation()
    const rect = surface.getBoundingClientRect()
    const dockRect = dock.getBoundingClientRect()
    mobileDrag.current = {
      offsetX: event.clientX - dockRect.left,
      offsetY: event.clientY - dockRect.top,
      startX: event.clientX,
      startY: event.clientY,
    }
    setMobilePos({
      x: dockRect.left - rect.left,
      y: dockRect.top - rect.top,
    })
    event.currentTarget.setPointerCapture(event.pointerId)
  }

  const mobileDockMove = (event: ReactPointerEvent<HTMLButtonElement>) => {
    const drag = mobileDrag.current
    const surface = surfaceRef.current
    const dock = mobileDockRef.current
    if (!drag || !surface || !dock) return
    const rect = surface.getBoundingClientRect()
    const width = dock.offsetWidth
    const height = dock.offsetHeight
    setMobilePos({
      x: Math.min(
        Math.max(event.clientX - rect.left - drag.offsetX, 8),
        Math.max(8, rect.width - width - 8),
      ),
      y: Math.min(
        Math.max(event.clientY - rect.top - drag.offsetY, 8),
        Math.max(8, rect.height - height - 8),
      ),
    })
  }

  const mobileDockUp = (event: ReactPointerEvent<HTMLButtonElement>) => {
    const drag = mobileDrag.current
    const surface = surfaceRef.current
    const dock = mobileDockRef.current
    mobileDrag.current = null
    if (!drag || !surface || !dock) return
    const rect = surface.getBoundingClientRect()
    const width = dock.offsetWidth
    const height = dock.offsetHeight
    const x = event.clientX - rect.left - drag.offsetX
    const y = event.clientY - rect.top - drag.offsetY
    setMobilePos({
      x: Math.min(
        Math.max(x + width / 2 < rect.width / 2 ? 8 : rect.width - width - 8, 8),
        Math.max(8, rect.width - width - 8),
      ),
      y: Math.min(
        Math.max(y, 8),
        Math.max(8, rect.height - height - 8),
      ),
    })
  }

  const onTouchpadDown = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (!tab.controlEnabled || !inputAvailable) return
    event.preventDefault()
    event.currentTarget.focus()
    event.currentTarget.setPointerCapture(event.pointerId)
    touchPoints.current.set(event.pointerId, {
      x: event.clientX,
      y: event.clientY,
    })

    if (touchPoints.current.size === 1) {
      touchStart.current = {
        time: Date.now(),
        x: event.clientX,
        y: event.clientY,
        moved: false,
      }
      suppressTap.current = false
      scrollAccum.current = { x: 0, y: 0 }
      lastCentroid.current = null
      cancelLongPress()
      longPressTimer.current = window.setTimeout(() => {
        longPressTimer.current = null
        if (touchPoints.current.size !== 1 || !touchStart.current ||
            touchStart.current.moved) {
          return
        }
        suppressTap.current = true
        sendVirtualClick(3)
        navigator.vibrate?.(12)
      }, 520)
    } else {
      cancelLongPress()
      suppressTap.current = true
      lastCentroid.current = touchCentroid()
      scrollAccum.current = { x: 0, y: 0 }
    }
  }

  const onTouchpadMove = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (!tab.controlEnabled || !inputAvailable) return
    const point = touchPoints.current.get(event.pointerId)
    if (!point) return
    event.preventDefault()
    const dx = event.clientX - point.x
    const dy = event.clientY - point.y
    point.x = event.clientX
    point.y = event.clientY

    if (touchPoints.current.size >= 2) {
      const centroid = touchCentroid()
      if (lastCentroid.current) {
        scrollAccum.current.x += centroid.x - lastCentroid.current.x
        scrollAccum.current.y += centroid.y - lastCentroid.current.y
        const step = 14
        const notchesX = Math.trunc(scrollAccum.current.x / step)
        const notchesY = Math.trunc(scrollAccum.current.y / step)
        if (notchesX !== 0 || notchesY !== 0) {
          scrollAccum.current.x -= notchesX * step
          scrollAccum.current.y -= notchesY * step
          connection()?.sendInput({
            k: 'wheel',
            dx: -notchesX,
            dy: -notchesY,
          })
        }
      }
      lastCentroid.current = centroid
      return
    }

    const start = touchStart.current
    if (start && !start.moved) {
      const distance = Math.hypot(
        event.clientX - start.x,
        event.clientY - start.y,
      )
      if (distance > 6) {
        start.moved = true
        cancelLongPress()
      }
    }

    const rect = surfaceRef.current?.getBoundingClientRect()
    if (!rect) return
    const sensitivity = 1.7
    virtualPos.current = {
      x: Math.min(
        1,
        Math.max(
          0,
          virtualPos.current.x +
            (dx / Math.max(1, rect.width)) * sensitivity,
        ),
      ),
      y: Math.min(
        1,
        Math.max(
          0,
          virtualPos.current.y +
            (dy / Math.max(1, rect.height)) * sensitivity,
        ),
      ),
    }
    sendVirtualMove()
  }

  const onTouchpadUp = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (!tab.controlEnabled) return
    if (event.currentTarget.hasPointerCapture(event.pointerId)) {
      event.currentTarget.releasePointerCapture(event.pointerId)
    }
    const start = touchStart.current
    touchPoints.current.delete(event.pointerId)
    cancelLongPress()

    if (touchPoints.current.size > 0) {
      lastCentroid.current = null
      return
    }

    if (
      start &&
      !start.moved &&
      !suppressTap.current &&
      Date.now() - start.time < 520
    ) {
      sendVirtualClick(1)
    }
    touchStart.current = null
    suppressTap.current = false
    lastCentroid.current = null
    scrollAccum.current = { x: 0, y: 0 }
  }

  const onTouchpadCancel = (event: ReactPointerEvent<HTMLDivElement>) => {
    touchPoints.current.delete(event.pointerId)
    cancelLongPress()
    if (touchPoints.current.size === 0) {
      touchStart.current = null
      suppressTap.current = false
      lastCentroid.current = null
      scrollAccum.current = { x: 0, y: 0 }
    }
  }

  const onPointerDown = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (!tab.controlEnabled || !inputAvailable) return
    if (virtualMouse) {
      onTouchpadDown(event)
      return
    }
    const point = normalize(event.clientX, event.clientY)
    if (!point) return
    event.preventDefault()
    event.currentTarget.focus()
    event.currentTarget.setPointerCapture(event.pointerId)
    connection()?.sendInput({ k: 'move', ...point })
    if (event.button <= 2) {
      const button = event.button + 1
      pressedButtons.current.add(button)
      connection()?.sendInput({ k: 'button', button, down: true })
    }
  }

  const onPointerUp = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (!inputAvailable) return
    if (virtualMouse) {
      onTouchpadUp(event)
      return
    }
    if (event.currentTarget.hasPointerCapture(event.pointerId)) {
      event.currentTarget.releasePointerCapture(event.pointerId)
    }
    if (event.button <= 2) {
      const button = event.button + 1
      pressedButtons.current.delete(button)
      connection()?.sendInput({ k: 'button', button, down: false })
    }
  }

  const onPointerMove = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (!tab.controlEnabled || !inputAvailable) return
    if (virtualMouse) {
      onTouchpadMove(event)
      return
    }
    if (displayMode === 'actual') {
      panPointer.current = { x: event.clientX, y: event.clientY }
    }
    const point = normalize(event.clientX, event.clientY)
    if (!point) return
    pendingMove.current = point
    if (moveScheduled.current) return
    moveScheduled.current = true
    window.requestAnimationFrame(() => {
      moveScheduled.current = false
      const next = pendingMove.current
      if (next) connection()?.sendInput({ k: 'move', ...next })
    })
  }

  useEffect(() => {
    const element = surfaceRef.current
    if (!element || !tab.controlEnabled || !inputAvailable) return
    const onWheel = (event: WheelEvent) => {
      // In 1:1 mode the wheel scrolls the local oversized image; sending it to
      // the remote as well would defeat the purpose of that mode.
      if (displayMode === 'actual') return
      event.preventDefault()
      const dy = event.deltaY === 0 ? 0 : event.deltaY > 0 ? 1 : -1
      const dx = event.deltaX === 0 ? 0 : event.deltaX > 0 ? 1 : -1
      if (dx !== 0 || dy !== 0) connection()?.sendInput({ k: 'wheel', dx, dy })
    }
    element.addEventListener('wheel', onWheel, { passive: false })
    return () => element.removeEventListener('wheel', onWheel)
  }, [tab.controlEnabled, inputAvailable, tab.id, connection, displayMode])

  useEffect(() => {
    if (!tab.controlEnabled) return
    const keys = pressedKeys.current
    const buttons = pressedButtons.current
    const codeFor = (event: KeyboardEvent) =>
      event.code && event.code !== 'Unidentified'
        ? event.code
        : keyToCode(event.key)
    const down = (event: KeyboardEvent) => {
      if (event.repeat || event.isComposing) return
      if (isLocalInputTarget(event.target)) return
      // Settings and other local dialogs keep their shortcuts (Esc closes
      // them) even when focus happens to sit on the page background.
      if (useAppStore.getState().settingsOpen) return
      const code = codeFor(event)
      if (!code) return
      event.preventDefault()
      keys.add(code)
      connection()?.sendInput({ k: 'key', code, down: true })
    }
    const up = (event: KeyboardEvent) => {
      if (event.isComposing) return
      const code = codeFor(event)
      if (!code) return
      // A remote key may be released after focus moved into a local field;
      // still release it so the remote does not keep it held down.
      if (isLocalInputTarget(event.target) && !keys.has(code)) return
      event.preventDefault()
      keys.delete(code)
      connection()?.sendInput({ k: 'key', code, down: false })
    }
    const releaseAll = () => {
      for (const code of keys) {
        connection()?.sendInput({ k: 'key', code, down: false })
      }
      keys.clear()
      for (const button of buttons) {
        connection()?.sendInput({ k: 'button', button, down: false })
      }
      buttons.clear()
    }
    // Capture phase: a focused local widget or a stopPropagation() elsewhere
    // in the page must not swallow remote keys. The target guard above keeps
    // chat/settings typing local.
    window.addEventListener('keydown', down, true)
    window.addEventListener('keyup', up, true)
    window.addEventListener('blur', releaseAll)
    return () => {
      window.removeEventListener('keydown', down, true)
      window.removeEventListener('keyup', up, true)
      window.removeEventListener('blur', releaseAll)
      // Release anything still held on the host, but keep the user's control
      // choice: this effect also unwinds when the view is unmounted for a tab
      // switch, and turning control off there made every switch lose it.
      releaseAll()
    }
  }, [tab.controlEnabled, tab.id, connection])

  // Keyboard capture is global, but focusing the surface when control starts
  // (or when the window regains focus) keeps the remote page the active
  // target and avoids keys landing on the surrounding chrome.
  useEffect(() => {
    if (!tab.controlEnabled) return
    const focusSurface = () => {
      const surface = surfaceRef.current
      if (!surface) return
      const active = document.activeElement
      if (active === null || active === document.body) {
        surface.focus({ preventScroll: true })
      }
    }
    focusSurface()
    const timer = window.setTimeout(focusSurface, 100)
    window.addEventListener('focus', focusSurface)
    return () => {
      window.clearTimeout(timer)
      window.removeEventListener('focus', focusSurface)
    }
  }, [tab.controlEnabled, tab.id])

  const copyFromRemote = () => {
    setActionError(null)
    connection()?.requestClipboard()
  }

  const sendToRemote = () => {
    if (!navigator.clipboard?.readText) {
      setActionError(String(t('error.clipboard_unavailable')))
      return
    }
    navigator.clipboard
      .readText()
      .then((text) => connection()?.setClipboard(text))
      .catch(() => setActionError(String(t('error.clipboard_read_failed'))))
  }

  const openPopout = () => {
    if (
      openSessionWindow({
        host: tab.host,
        port: tab.port,
        token: tab.token,
        label: tab.label,
      })
    ) {
      closeTab(tab.id)
    }
  }

  const latency =
    tab.stats.latencyMs === null ? '—' : `${Math.round(tab.stats.latencyMs)} ms`
  const syntheticLabel = synthetic ? t('session.syntheticShort') : null
  const displays = tab.welcome?.displays ?? []
  // Live path from the local core's proxy first, then what the host's welcome
  // reported, then a legacy mapping (older cores mixed "relay"/"udp" into the
  // single transport field).
  const welcomeTransport = tab.welcome?.transport
  const livePath =
    tab.path ??
    (tab.transport === 'relay'
      ? 'relay'
      : tab.transport === 'p2p'
        ? 'p2p'
        : undefined)
  const legacyPath =
    welcomeTransport === 'relay'
      ? 'relay'
      : welcomeTransport === 'udp'
        ? 'p2p'
        : 'direct'
  const pathName = livePath ?? tab.welcome?.transport_path ?? legacyPath
  const pathLabel = PATH_LABELS[pathName] ?? pathName.toUpperCase()
  const protocolName =
    tab.protocol ??
    (welcomeTransport === 'relay' ? 'websocket' : welcomeTransport)
  const protocolLabel = protocolName
    ? (PROTOCOL_LABELS[protocolName] ?? protocolName.toUpperCase())
    : '—'
  // Follow the frame actually on screen; the welcome codec is only a snapshot
  // from connect time and goes stale when the preset changes.
  // The tile transport picks per frame whether zlib helps, so the wire codec
  // flips between `bgra` and `deflate-bgra`. The watermark shows the pixel
  // format instead: the compression choice is not something to read off it.
  const codecLabel = tab.frame
    ? codecName(tab.frame.codec).replace(/^deflate-/, '')
    : (tab.welcome?.tiles.codec ?? '')
  // The session transport names the HTTP leg directly: http2/http3 are the
  // RFC 8441/9220 WebSockets, websocket is the h1 baseline, and quic/tcp/udp
  // are raw MOLK streams without an HTTP version.
  const httpVersionLabel =
    protocolName === 'http2'
      ? 'HTTP/2'
      : protocolName === 'http3'
        ? 'HTTP/3'
        : protocolName === 'websocket'
          ? 'HTTP/1.1'
          : '—'
  // Follow the frame that is actually on screen: switching codecs (or a drop
  // back to tiles) shows up in the watermark on the next frame, not only when
  // the host restarts the stream.
  const frameCodec = tab.frame?.codec
  const encodingLabel =
    frameCodec === Codec.h264Stream
      ? 'H.264'
      : frameCodec === Codec.vp9Stream
        ? 'VP9'
        : frameCodec === Codec.vp8Stream
          ? 'VP8'
          : frameCodec === Codec.av1Stream
            ? 'AV1'
            : frameCodec === undefined && tab.streamCodec
              ? tab.streamCodec === 'h264'
                ? 'H.264'
                : tab.streamCodec.toUpperCase()
              : t('session.watermarkTiles')

  return (
    <div ref={sessionRootRef} className="flex min-h-0 flex-1 flex-col">
      {tab.error || actionError ? (
        <div
          className={
            (isFullscreen ? 'hidden' : 'flex flex-wrap items-center gap-2') +
            ' border-b border-danger/30 bg-danger/10 px-4 py-2 text-[12px] text-danger'
          }
        >
          <span className="min-w-0 flex-1">
            {needsCode
              ? t('session.badCode')
              : tab.errorCode === 'display_mode_busy'
                ? t('session.displayModeBusy')
                : displayModeError
                  ? t('session.displayModeFailed')
                  : tab.error
                    ? String(
                        t(`error.${tab.error}`, { defaultValue: tab.error }),
                      )
                    : actionError}
          </span>
          {needsCode ? (
            <form
              className="flex items-center gap-1"
              onSubmit={(event) => {
                event.preventDefault()
                submitCode()
              }}
            >
              <input
                className="field h-7 w-32 px-2 font-mono text-[11px]"
                value={retryCode}
                onChange={(event) => setRetryCode(event.target.value)}
                placeholder={t('session.enterCode')}
                spellCheck={false}
                autoFocus
              />
              <button
                type="submit"
                className="btn h-7 px-2 text-[11px]"
                disabled={retryCode.trim() === ''}
              >
                {t('session.connect')}
              </button>
            </form>
          ) : null}
          {tab.error && !needsCode && !displayModeError ? (
            <button
              type="button"
              onClick={reconnect}
              className="btn h-7 px-2 text-[11px]"
            >
              {t('session.reconnect')}
            </button>
          ) : null}
          {displayModeError ? (
            <button
              type="button"
              onClick={() => connection()?.dismissError()}
              className="btn h-7 px-2 text-[11px]"
            >
              {t('error.dismiss')}
            </button>
          ) : null}
          {tab.error && !needsCode && /token|unauth|auth|invalid/i.test(tab.error) ? (
            <button
              type="button"
              onClick={requestAccessFromError}
              className="btn h-7 px-2 text-[11px]"
              title={t('home.requestAccessHint')}
            >
              {t('session.requestAccess')}
            </button>
          ) : null}
        </div>
      ) : null}

      <div
        className={
          isFullscreen
            ? 'flex min-h-0 flex-1'
            : 'flex min-h-0 flex-1 items-center justify-center p-3 sm:p-4'
        }
      >
        <div
          ref={surfaceRef}
          tabIndex={-1}
          onPointerDown={onPointerDown}
          onPointerUp={onPointerUp}
          onPointerMove={onPointerMove}
          onPointerCancel={onTouchpadCancel}
          onPointerLeave={() => {
            panPointer.current = null
          }}
          // Files are dropped on the transfer panel's own drop zone, not on
          // the picture: a stray drop over the remote desktop must not start a
          // transfer, and a browser shell may not even deliver it.
          onContextMenu={(event) => {
            if (tab.controlEnabled) event.preventDefault()
          }}
          className={
            'relative h-full w-full touch-none select-none outline-none transition ' +
            (isFullscreen
              ? 'bg-canvas ' +
                (displayMode === 'actual'
                  ? 'overflow-auto'
                  : 'overflow-hidden')
              : 'rounded-xl border bg-black/70 ' +
                (displayMode === 'actual'
                  ? 'overflow-auto '
                  : 'flex items-center justify-center overflow-hidden ') +
                (tab.controlEnabled ? 'border-accent/45' : 'border-line'))
          }
        >
          {tab.frame ? (
            <div
              className={
                displayMode === 'actual'
                  ? 'relative'
                  : 'relative h-full w-full'
              }
              style={{
                ...(displayMode === 'actual'
                  ? {
                      width: tab.frame.screenW,
                      height: tab.frame.screenH,
                    }
                  : {}),
                ...(tab.phase === 'live' && tab.controlEnabled && !virtualMouse
                  ? { cursor: remoteCursorCss ?? 'default' }
                  : {}),
              }}
            >
              <TileCanvas
                frame={tab.frame}
                mode={displayMode}
                sessionKey={tab.id}
              />
              {(!tab.controlEnabled || virtualMouse) && !synthetic ? (
                <RemoteCursor
                  shape={tab.cursorShape}
                  position={tab.cursorPosition}
                  welcome={welcome}
                  viewWidth={surfaceSize.width}
                  viewHeight={surfaceSize.height}
                  screenWidth={tab.frame?.screenW}
                  screenHeight={tab.frame?.screenH}
                  mode={displayMode}
                />
              ) : null}
              <div className="pointer-events-none absolute left-3 top-3 flex flex-wrap gap-1.5">
                {qualityWatermark && welcome ? (
                  <span className="rounded-md border border-white/15 bg-black/65 px-2 py-1 font-mono text-[10px] text-white/85 backdrop-blur">
                    {welcome.tiles.screen_w}×{welcome.tiles.screen_h}
                  </span>
                ) : null}
                {qualityWatermark && welcome ? (
                  <span className="rounded-md border border-white/15 bg-black/65 px-2 py-1 font-mono text-[10px] uppercase text-white/85 backdrop-blur">
                    {codecLabel}
                  </span>
                ) : null}
                {qualityWatermark && welcome ? (
                  <span className="rounded-md border border-accent/40 bg-black/65 px-2 py-1 font-mono text-[10px] text-accent backdrop-blur">
                    {encodingLabel}
                  </span>
                ) : null}
                {qualityWatermark && welcome ? (
                  <span className="rounded-md border border-white/15 bg-black/65 px-2 py-1 font-mono text-[10px] text-white/85 backdrop-blur">
                    {t('session.watermarkLink')} {pathLabel} · {protocolLabel} ·{' '}
                    {httpVersionLabel}
                  </span>
                ) : null}
                {qualityWatermark && welcome?.tiles.backend ? (
                  <span className="rounded-md border border-white/15 bg-black/65 px-2 py-1 font-mono text-[10px] text-white/85 backdrop-blur">
                    {welcome.tiles.backend}
                    {welcome.version ? ` · v${welcome.version}` : ''}
                  </span>
                ) : null}
                {syntheticLabel ? (
                  <span className="rounded-md border border-warn/40 bg-warn/10 px-2 py-1 font-mono text-[10px] text-warn backdrop-blur">
                    {syntheticLabel}
                  </span>
                ) : null}
                {tab.controlEnabled && inputAvailable ? (
                  <span className="rounded-md border border-accent/50 bg-accent-soft px-2 py-1 font-mono text-[10px] text-accent backdrop-blur">
                    {t('session.controlOn')}
                  </span>
                ) : null}
              </div>

              {qualityWatermark ? (
                <div className="pointer-events-none absolute bottom-3 right-3 flex items-center gap-2 rounded-md border border-white/15 bg-black/65 px-2.5 py-1 font-mono text-[10px] text-white/80 backdrop-blur">
                  <span className="text-accent">{tab.stats.fps} fps</span>
                  <span>{latency}</span>
                  <span>{formatBandwidth(tab.stats.bytesPerSec)}</span>
                  <span className="text-accent">{encodingLabel}</span>
                  <span className="text-muted/60">
                    {pathLabel}/{protocolLabel}/{httpVersionLabel}
                  </span>
                </div>
              ) : null}
            </div>
          ) : (
            <div className="absolute inset-0 flex items-center justify-center gap-2 text-sm text-muted">
              <Radio size={15} className="animate-pulse text-accent" />
              {tab.phase === 'connecting'
                ? tab.notice === 'waiting_approval'
                  ? (
                    <span className="flex flex-col items-center gap-2">
                      <span className="flex items-center gap-2">
                        <Radio size={15} className="animate-pulse text-accent" />
                        {t('session.waitingApproval')}
                        {approvalLeft !== null
                          ? ` · ${approvalLeft}s`
                          : ''}
                      </span>
                      <button
                        type="button"
                        onClick={cancelApproval}
                        className="btn h-7 px-2 text-[11px]"
                      >
                        <X size={12} />
                        {t('session.cancelRequest')}
                      </button>
                    </span>
                  )
                  : t('session.connecting')
                : t('session.waiting')}
            </div>
          )}

          {tab.phase === 'disconnected' ? (
            <div className="absolute inset-0 z-20 flex flex-col items-center justify-center gap-2 rounded-xl bg-black/65 px-4 text-center">
              <div className="text-base font-semibold text-ink">
                {t('session.disconnected')}
              </div>
              {retryLeft !== null ? (
                <div className="text-[12px] text-muted">
                  {t('session.retryIn', { seconds: retryLeft })}
                </div>
              ) : null}
              <div className="mt-1 flex gap-2">
                <button
                  type="button"
                  className="btn btn-primary h-8"
                  onClick={() => connection()?.reconnectNow()}
                >
                  {t('session.retryNow')}
                </button>
                <button
                  type="button"
                  className="btn h-8"
                  onClick={() => {
                    connection()?.disconnect()
                    closeTab(tab.id)
                  }}
                >
                  {t('session.stopRetry')}
                </button>
              </div>
            </div>
          ) : null}

          {quickOpen ? (
            <div
              ref={quickBarRef}
              className="pointer-events-auto absolute left-1/2 top-3 z-40 flex w-max max-w-[96vw] -translate-x-1/2 items-center gap-0.5 overflow-x-auto rounded-full border border-line bg-surface/95 px-1.5 py-1 text-ink shadow-lg backdrop-blur"
              onPointerDown={(event) => event.stopPropagation()}
              onPointerUp={(event) => event.stopPropagation()}
              onBlur={(event) => {
                if (barPinned) return
                const next = event.relatedTarget as Element | null
                if (next?.closest?.('[data-toolbar-menu]')) return
                if (!event.currentTarget.contains(next as Node | null)) {
                  setQuickOpen(false)
                }
              }}
            >
              <button
                type="button"
                onClick={() => {
                  const next = !barPinned
                  setBarPinned(next)
                  try {
                    window.localStorage.setItem(
                      'molink.toolbarPinned',
                      next ? '1' : '0',
                    )
                  } catch {
                    // The pin still works for this session.
                  }
                }}
                className={
                  'btn h-7 w-7 shrink-0 !px-0 ' +
                  (barPinned ? 'text-accent' : 'btn-quiet')
                }
                title={
                  barPinned ? t('session.unpinToolbar') : t('session.pinToolbar')
                }
                aria-label={
                  barPinned ? t('session.unpinToolbar') : t('session.pinToolbar')
                }
                aria-pressed={barPinned}
              >
                {barPinned ? <PinOff size={13} /> : <Pin size={13} />}
              </button>
              <button
                type="button"
                onClick={() => {
                  setBarPinned(false)
                  try {
                    window.localStorage.setItem('molink.toolbarPinned', '0')
                  } catch {
                    // Ignore: closing matters more than remembering it.
                  }
                  setQuickOpen(false)
                }}
                className="btn btn-quiet h-7 w-7 shrink-0 !px-0"
                title={t('session.quickActions')}
                aria-label={t('session.quickActions')}
              >
                <X size={14} />
              </button>
              {isFullscreen ? (
                <span
                  className="chip max-w-[8rem] shrink-0 truncate !px-1.5 !py-0 text-[10px]"
                  title={tab.label}
                >
                  {tab.label}
                </span>
              ) : null}
              <button
                type="button"
                onClick={toggleFullscreen}
                className="btn btn-quiet h-7 w-7 shrink-0 !px-0"
                title={
                  isFullscreen
                    ? t('session.exitFullscreen')
                    : t('session.fullscreen')
                }
                aria-label={
                  isFullscreen
                    ? t('session.exitFullscreen')
                    : t('session.fullscreen')
                }
              >
                {isFullscreen ? (
                  <Minimize2 size={14} />
                ) : (
                  <Maximize2 size={14} />
                )}
              </button>
              <span className="mx-0.5 h-4 w-px shrink-0 bg-line" />
              {(['fit', 'actual', 'fill'] as DisplayMode[]).map((mode) => (
                <button
                  key={mode}
                  type="button"
                  onClick={() => changeDisplayMode(mode)}
                  className={
                    'btn h-7 shrink-0 px-2 text-[11px] ' +
                    (displayMode === mode ? 'btn-primary' : 'btn-quiet')
                  }
                >
                  {t(`session.${mode}`)}
                </button>
              ))}
              <label
                className="flex shrink-0 items-center gap-1 rounded-md border border-line bg-panel px-1.5 text-muted"
                title={t('session.frameRate')}
              >
                <Activity size={12} />
                <select
                  value={String(frameRate)}
                  onChange={(event) =>
                    setFrameRate(parseFpsSetting(event.target.value))
                  }
                  aria-label={t('session.frameRate')}
                  className="h-7 bg-transparent text-[11px] text-ink outline-none [&>option]:bg-surface [&>option]:text-ink"
                >
                  {FPS_CHOICES.map((choice) => (
                    <option key={String(choice)} value={String(choice)}>
                      {choice === 'auto'
                        ? t('session.frameRateAuto')
                        : t('session.frameRateValue', { count: choice })}
                    </option>
                  ))}
                </select>
              </label>
              <label
                className="flex shrink-0 items-center gap-1 rounded-md border border-line bg-panel px-1.5 text-muted"
                title={t('session.quality')}
              >
                <Gauge size={12} />
                <select
                  value={qualityMode}
                  onChange={(event) =>
                    setQualityMode(event.target.value as QualityMode)
                  }
                  aria-label={t('session.quality')}
                  className="h-7 bg-transparent text-[11px] text-ink outline-none [&>option]:bg-surface [&>option]:text-ink"
                >
                  <option value="turbo">{t('session.qualityTurbo')}</option>
                  <option value="smooth">{t('session.qualitySmooth')}</option>
                  <option value="balanced">{t('session.qualityBalanced')}</option>
                  <option value="quality">{t('session.qualityHigh')}</option>
                  <option value="ultra">{t('session.qualityUltra')}</option>
                  <option value="lossless">{t('session.qualityLossless')}</option>
                  <option value="game">{t('session.qualityGame')}</option>
                  <option value="auto">{t('session.qualityAuto')}</option>
                </select>
              </label>
              {codecOptions.length > 0 ? (
                <label
                  className="flex shrink-0 items-center gap-1 rounded-md border border-line bg-panel px-1.5 text-muted"
                  title={t('session.codec')}
                >
                  <Cpu size={12} />
                  <select
                    value={codecPreference}
                    onChange={(event) =>
                      connection()?.setStreamCodec(event.target.value)
                    }
                    aria-label={t('session.codec')}
                    className="h-7 bg-transparent text-[11px] text-ink outline-none [&>option]:bg-surface [&>option]:text-ink"
                  >
                    <option value="auto">{t('session.codecAuto')}</option>
                    {codecOptions.map((codec) => (
                      <option key={codec} value={codec}>
                        {codec === 'tiles'
                          ? t('session.codecTiles')
                          : codec === 'h264'
                            ? 'H.264'
                            : codec.toUpperCase()}
                      </option>
                    ))}
                  </select>
                </label>
              ) : null}
              <button
                type="button"
                onClick={() => setQualityWatermark(!qualityWatermark)}
                className={
                  'btn h-7 w-7 shrink-0 !px-0 ' +
                  (qualityWatermark ? 'btn-primary' : 'btn-quiet')
                }
                title={t('settings.watermark')}
                aria-label={t('settings.watermark')}
                aria-pressed={qualityWatermark}
              >
                <Activity size={13} />
              </button>
              {displays.length > 1 ? (
                <label
                  className="flex shrink-0 items-center gap-1 rounded-md border border-line bg-panel px-1.5 text-muted"
                  title={t('session.display')}
                >
                  <Monitor size={12} />
                  <select
                    value={tab.displayIndex}
                    onChange={(event) =>
                      connection()?.setDisplay(Number(event.target.value))
                    }
                    aria-label={t('session.display')}
                    className="h-7 max-w-[8rem] bg-transparent text-[11px] text-ink outline-none [&>option]:bg-surface [&>option]:text-ink"
                  >
                    {displays.map((display) => (
                      <option key={display.index} value={display.index}>
                        {display.name || `Display ${display.index + 1}`}
                      </option>
                    ))}
                  </select>
                </label>
              ) : null}
              {welcome && !welcome.tiles.synthetic &&
              welcome.tiles.can_set_display_mode !== false ? (
                <label
                  className="flex shrink-0 items-center gap-1 rounded-md border border-line bg-panel px-1.5 text-muted"
                  title={t('session.resolution')}
                >
                  <Monitor size={12} />
                  <select
                    value={`${tab.frame?.screenW ?? welcome.tiles.screen_w}x${
                      tab.frame?.screenH ?? welcome.tiles.screen_h
                    }`}
                    onChange={(event) => {
                      const [width, height] = event.target.value
                        .split('x')
                        .map(Number)
                      if (width && height) {
                        connection()?.setDisplayMode(width, height)
                      }
                    }}
                    aria-label={t('session.resolution')}
                    className="h-7 bg-transparent text-[11px] text-ink outline-none [&>option]:bg-surface [&>option]:text-ink"
                  >
                    {[
                      `${tab.frame?.screenW ?? welcome.tiles.screen_w}x${
                        tab.frame?.screenH ?? welcome.tiles.screen_h
                      }`,
                      '1280x720',
                      '1920x1080',
                      '2560x1440',
                      '3840x2160',
                    ]
                      .filter((value, index, all) => all.indexOf(value) === index)
                      .map((value) => (
                        <option key={value} value={value}>
                          {value}
                        </option>
                      ))}
                  </select>
                </label>
              ) : null}
              <span className="mx-0.5 h-4 w-px shrink-0 bg-line" />
              {clipboardAvailable && !compactBar ? (
                <>
                  <button
                    type="button"
                    onClick={() =>
                      setClipboardEnabled(tab.id, !tab.clipboardEnabled)
                    }
                    className={
                      'btn h-7 w-7 shrink-0 !px-0 ' +
                      (tab.clipboardEnabled ? 'text-accent' : 'btn-quiet')
                    }
                    title={
                      tab.clipboardEnabled
                        ? t('session.clipboardOff')
                        : t('session.clipboardOn')
                    }
                    aria-label={
                      tab.clipboardEnabled
                        ? t('session.clipboardOff')
                        : t('session.clipboardOn')
                    }
                    aria-pressed={tab.clipboardEnabled}
                  >
                    {tab.clipboardEnabled ? (
                      <ClipboardCheck size={13} />
                    ) : (
                      <ClipboardX size={13} />
                    )}
                  </button>
                  <button
                    type="button"
                    onClick={copyFromRemote}
                    disabled={!tab.clipboardEnabled}
                    className="btn btn-quiet h-7 w-7 shrink-0 !px-0"
                    title={t('session.clipboardGet')}
                    aria-label={t('session.clipboardGet')}
                  >
                    <ClipboardCopy size={13} />
                  </button>
                  <button
                    type="button"
                    onClick={sendToRemote}
                    disabled={!tab.clipboardEnabled}
                    className="btn btn-quiet h-7 w-7 shrink-0 !px-0"
                    title={t('session.clipboardSet')}
                    aria-label={t('session.clipboardSet')}
                  >
                    <ClipboardPaste size={13} />
                  </button>
                </>
              ) : null}
              {clipboardAvailable && compactBar ? (
                <ToolbarMenu
                  icon={
                    tab.clipboardEnabled ? (
                      <ClipboardCheck size={13} />
                    ) : (
                      <ClipboardX size={13} />
                    )
                  }
                  label={t('session.clipboard')}
                  title={t('session.clipboard')}
                  items={[
                    {
                      key: 'toggle',
                      label: tab.clipboardEnabled
                        ? t('session.clipboardOff')
                        : t('session.clipboardOn'),
                      icon: tab.clipboardEnabled ? (
                        <ClipboardCheck size={12} />
                      ) : (
                        <ClipboardX size={12} />
                      ),
                      active: tab.clipboardEnabled,
                      onSelect: () =>
                        setClipboardEnabled(tab.id, !tab.clipboardEnabled),
                    },
                    {
                      key: 'get',
                      label: t('session.clipboardGet'),
                      icon: <ClipboardCopy size={12} />,
                      disabled: !tab.clipboardEnabled,
                      onSelect: copyFromRemote,
                    },
                    {
                      key: 'set',
                      label: t('session.clipboardSet'),
                      icon: <ClipboardPaste size={12} />,
                      disabled: !tab.clipboardEnabled,
                      onSelect: sendToRemote,
                    },
                  ]}
                />
              ) : null}
              {audioAvailable ? (
                <button
                  type="button"
                  onClick={toggleAudio}
                  className={
                    'btn h-7 w-7 shrink-0 !px-0 ' +
                    (audioOn ? 'text-accent' : 'btn-quiet')
                  }
                  title={audioOn ? t('session.audioOff') : t('session.audioOn')}
                  aria-label={
                    audioOn ? t('session.audioOff') : t('session.audioOn')
                  }
                  aria-pressed={audioOn}
                >
                  {audioOn ? <Volume2 size={13} /> : <VolumeX size={13} />}
                </button>
              ) : null}
              {audioAvailable && audioOn ? (
                <input
                  type="range"
                  min={0}
                  max={1}
                  step={0.05}
                  value={audioVolume}
                  onChange={(event) =>
                    changeAudioVolume(Number(event.target.value))
                  }
                  className="h-1 w-14 shrink-0 cursor-pointer"
                  style={{ accentColor: 'var(--accent)' }}
                  aria-label={t('session.audioVolume')}
                />
              ) : null}
              {compactBar ? (
                <ToolbarMenu
                  icon={<Wrench size={13} />}
                  label={t('session.tools')}
                  title={t('session.tools')}
                  items={[
                    {
                      key: 'chat',
                      label: t('session.chat'),
                      icon: <MessageSquare size={12} />,
                      active: chatOpen,
                      onSelect: () => {
                        setChatUnread(0)
                        setChatOpen(true)
                      },
                    },
                    {
                      key: 'files',
                      label: t('session.files'),
                      icon: <FolderOpen size={12} />,
                      active: filesOpen,
                      onSelect: () => {
                        setFilesOpen(true)
                        setChatOpen(false)
                      },
                    },
                    ...(inputAvailable
                      ? [
                          {
                            key: 'control',
                            label: tab.controlEnabled
                              ? t('session.stopControl')
                              : t('session.startControl'),
                            icon: tab.controlEnabled ? (
                              <Hand size={12} />
                            ) : (
                              <MousePointer2 size={12} />
                            ),
                            active: tab.controlEnabled,
                            onSelect: () => {
                              const next = !tab.controlEnabled
                              setControlEnabled(tab.id, next)
                              if (!next) setVirtualMouse(false)
                            },
                          },
                          {
                            key: 'sas',
                            label: t('session.ctrlAltDel'),
                            icon: <Keyboard size={12} />,
                            onSelect: () => connection()?.sendInput({ k: 'sas' }),
                          },
                        ]
                      : []),
                    ...(!popout && localMode
                      ? [
                          {
                            key: 'popout',
                            label: t('session.popOut'),
                            icon: <ExternalLink size={12} />,
                            onSelect: openPopout,
                          },
                        ]
                      : []),
                  ]}
                />
              ) : null}
              {!compactBar ? (
                <>
              <button
                type="button"
                onClick={() => {
                  setChatUnread(0)
                  setChatOpen(true)
                }}
                className={
                  'relative btn h-7 w-7 shrink-0 !px-0 ' +
                  (chatOpen ? 'text-accent' : 'btn-quiet')
                }
                title={t('session.chat')}
                aria-label={t('session.chat')}
              >
                <MessageSquare size={13} />
                {chatUnread > 0 && !chatOpen ? (
                  <span className="absolute -right-0.5 -top-0.5 grid h-4 min-w-4 place-items-center rounded-full bg-danger px-1 text-[9px] font-semibold text-white">
                    {chatUnread > 9 ? '9+' : chatUnread}
                  </span>
                ) : null}
              </button>
              <button
                type="button"
                onClick={() => {
                  setFilesOpen(true)
                  setChatOpen(false)
                }}
                className={
                  'btn h-7 w-7 shrink-0 !px-0 ' +
                  (filesOpen ? 'text-accent' : 'btn-quiet')
                }
                title={t('session.files')}
                aria-label={t('session.files')}
              >
                <FolderOpen size={13} />
              </button>
              {inputAvailable ? (
                <>
                  <button
                    type="button"
                    onClick={() => {
                      const next = !tab.controlEnabled
                      setControlEnabled(tab.id, next)
                      if (!next) setVirtualMouse(false)
                    }}
                    className={
                      'btn h-7 w-7 shrink-0 !px-0 ' +
                      (tab.controlEnabled
                        ? 'border-accent/50 bg-accent-soft text-accent'
                        : 'btn-quiet')
                    }
                    title={
                      tab.controlEnabled
                        ? t('session.stopControl')
                        : t('session.startControl')
                    }
                    aria-label={
                      tab.controlEnabled
                        ? t('session.stopControl')
                        : t('session.startControl')
                    }
                    aria-pressed={tab.controlEnabled}
                  >
                    {tab.controlEnabled ? (
                      <Hand size={13} />
                    ) : (
                      <MousePointer2 size={13} />
                    )}
                  </button>
                  <button
                    type="button"
                    onClick={() => connection()?.sendInput({ k: 'sas' })}
                    className="btn btn-quiet h-7 w-7 shrink-0 !px-0"
                    title={t('session.ctrlAltDelHint')}
                    aria-label={t('session.ctrlAltDel')}
                  >
                    <Keyboard size={13} />
                  </button>
                </>
              ) : null}
              {!popout && localMode ? (
                <button
                  type="button"
                  onClick={openPopout}
                  className="btn btn-quiet h-7 w-7 shrink-0 !px-0"
                  title={t('session.popOut')}
                  aria-label={t('session.popOut')}
                >
                  <ExternalLink size={13} />
                </button>
              ) : null}
                </>
              ) : null}
              <span className="mx-0.5 h-4 w-px shrink-0 bg-line" />
              {compactBar ? (
                <ToolbarMenu
                  icon={theme === 'dark' ? <Sun size={13} /> : <Moon size={13} />}
                  label={t('session.appearance')}
                  title={t('session.appearance')}
                  items={[
                    {
                      key: 'theme',
                      label:
                        theme === 'dark'
                          ? t('settings.themeLight')
                          : t('settings.themeDark'),
                      icon: theme === 'dark' ? <Sun size={12} /> : <Moon size={12} />,
                      onSelect: toggleTheme,
                    },
                    {
                      key: 'settings',
                      label: t('settings.title'),
                      icon: <Settings2 size={12} />,
                      onSelect: () => setSettingsOpen(true),
                    },
                  ]}
                />
              ) : (
                <>
                  <button
                    type="button"
                    onClick={toggleTheme}
                    className="btn btn-quiet h-7 w-7 shrink-0 !px-0"
                    title={theme === 'dark' ? t('settings.themeLight') : t('settings.themeDark')}
                    aria-label={theme === 'dark' ? t('settings.themeLight') : t('settings.themeDark')}
                  >
                    {theme === 'dark' ? <Sun size={13} /> : <Moon size={13} />}
                  </button>
                  <button
                    type="button"
                    onClick={() => setSettingsOpen(true)}
                    className="btn btn-quiet h-7 w-7 shrink-0 !px-0"
                    title={t('settings.title')}
                    aria-label={t('settings.title')}
                  >
                    <Settings2 size={13} />
                  </button>
                </>
              )}
              <span className="mx-0.5 h-4 w-px shrink-0 bg-line" />
              {compactBar ? (
                <span
                  className="chip !px-1.5 !py-0 text-[10px]"
                  title={`${t('session.path')}: ${pathLabel} · ${t('session.protocol')}: ${protocolLabel}`}
                >
                  {pathLabel}·{protocolLabel}
                </span>
              ) : (
                <>
                  <span className="chip !px-1.5 !py-0 text-[10px]" title={t('session.path')}>
                    {pathLabel}
                  </span>
                  <span className="chip !px-1.5 !py-0 text-[10px]" title={t('session.protocol')}>
                    {protocolLabel}
                  </span>
                </>
              )}
              {tab.av1Active ? (
                <span
                  className="chip !px-1.5 !py-0 text-[10px] text-emerald-500"
                  title={`AV1 ${tab.av1WireBytes ?? 0} B wire → ${tab.av1DecodedBytes ?? 0} B decoded`}
                >
                  AV1
                </span>
              ) : null}
              {isFullscreen && tab.welcome?.security ? (
                <span
                  className={
                    'chip flex items-center gap-1 !px-1.5 !py-0 text-[10px] ' +
                    (tab.welcome.security === 'e2e'
                      ? 'text-sky-500'
                      : tab.welcome.security === 'relay'
                        ? 'text-amber-500'
                        : 'text-warn')
                  }
                  title={
                    tab.welcome.security === 'e2e'
                      ? t('session.secureE2e')
                      : tab.welcome.security === 'relay'
                        ? t('session.secureRelay')
                        : t('session.insecureHint')
                  }
                >
                  {tab.welcome.security === 'none' ? (
                    <ShieldOff size={11} />
                  ) : (
                    <ShieldCheck size={11} />
                  )}
                  {tab.welcome.security === 'e2e'
                    ? t('session.secureE2eShort')
                    : tab.welcome.security === 'relay'
                      ? t('session.secureRelayShort')
                      : t('session.insecureShort')}
                </span>
              ) : null}
              <span className="mx-0.5 h-4 w-px shrink-0 bg-line" />
              <button
                type="button"
                onClick={() => {
                  if (popout) {
                    if (shellInfo.enabled) windowAction('close')
                    else window.close()
                  } else {
                    closeTab(tab.id)
                  }
                }}
                className="btn btn-quiet h-7 w-7 shrink-0 !px-0 hover:!border-danger/50 hover:!text-danger"
                title={popout ? t('session.closeWindow') : t('session.disconnect')}
                aria-label={popout ? t('session.closeWindow') : t('session.disconnect')}
              >
                {popout ? <X size={14} /> : <LogOut size={13} />}
              </button>
            </div>
          ) : (
            <button
              type="button"
              onPointerDown={quickHandleDown}
              onPointerMove={quickHandleMove}
              onPointerUp={quickHandleUp}
              onPointerCancel={quickHandleUp}
              onClick={toggleQuickOpen}
              className="pointer-events-auto absolute z-40 grid h-10 w-10 cursor-move touch-none place-items-center rounded-full border border-line bg-surface/90 text-muted shadow-lg backdrop-blur hover:text-ink"
              style={{ left: quickHandlePos.x, top: quickHandlePos.y }}
              title={t('session.quickActions')}
              aria-label={t('session.quickActions')}
            >
              <Settings2 size={15} />
              {chatUnread > 0 && !chatOpen ? (
                <span className="absolute -right-0.5 -top-0.5 h-2.5 w-2.5 rounded-full bg-danger ring-2 ring-surface" />
              ) : null}
            </button>
          )}

          {callPhase !== 'idle' ? (
            <div
              ref={callBarRef}
              data-local-input="true"
              style={{ left: callBarPos.x, top: callBarPos.y }}
              className="pointer-events-auto absolute z-30 flex max-w-[calc(100%-1.5rem)] items-center gap-2 rounded-full border border-line bg-surface/95 px-2 py-1.5 text-[11px] shadow-lg backdrop-blur"
              onPointerDown={(event) => event.stopPropagation()}
              onPointerUp={(event) => event.stopPropagation()}
            >
              <button
                type="button"
                onPointerDown={callHandleDown}
                onPointerMove={callHandleMove}
                onPointerUp={callHandleUp}
                onPointerCancel={callHandleUp}
                className="grid h-6 w-4 shrink-0 cursor-move touch-none place-items-center rounded-full text-muted transition hover:bg-panel hover:text-ink"
                title={t('session.moveCallBar')}
                aria-label={t('session.moveCallBar')}
              >
                <GripHorizontal size={12} />
              </button>
              <span
                className={
                  callPhase === 'active' ? 'text-accent' : 'text-warn'
                }
              >
                {callPhase === 'active' ? (
                  <Phone size={13} />
                ) : (
                  <PhoneOff size={13} />
                )}
              </span>
              <span className="whitespace-nowrap">
                {callPhase === 'ringing'
                  ? t('call.calling')
                  : `${t('call.inCall')} ${formatCallTime(callSeconds)}`}
              </span>
              {callPhase === 'active' ? (
                <button
                  type="button"
                  onClick={toggleCallMute}
                  className={
                    'grid h-6 w-6 place-items-center rounded-full transition hover:bg-panel ' +
                    (callMuted ? 'text-danger' : 'text-muted hover:text-ink')
                  }
                  title={callMuted ? t('call.unmute') : t('call.mute')}
                  aria-label={callMuted ? t('call.unmute') : t('call.mute')}
                >
                  {callMuted ? <MicOff size={13} /> : <Mic size={13} />}
                </button>
              ) : null}
              {callNotice ? (
                <span className="whitespace-nowrap text-warn">
                  {t(`call.${callNotice}`)}
                </span>
              ) : null}
              {hostMic === false && callPhase === 'active' ? (
                <span className="hidden whitespace-nowrap text-warn sm:inline">
                  {t('call.hostNoMic')}
                </span>
              ) : null}
              <button
                type="button"
                onClick={hangupVoiceCall}
                className="flex items-center gap-1 rounded-full bg-danger/15 px-2 py-1 text-danger transition hover:bg-danger/25"
                title={t('call.hangup')}
              >
                <PhoneOff size={12} />
                {t('call.hangup')}
              </button>
            </div>
          ) : null}

          <ChatPanel
            open={chatOpen}
            onClose={() => setChatOpen(false)}
            messages={chatMessages}
            onSend={(text) => connection()?.sendChat(text)}
            remoteLabel={tab.label}
            remoteId={welcome?.device_id}
            selfSessionId={welcome?.session_id}
            callAvailable={callAvailable}
            callPhase={callPhase}
            onCall={startVoiceCall}
          />

          <FileTransferPanel
            open={filesOpen}
            onClose={() => setFilesOpen(false)}
            connection={connection() ?? null}
          />


          {touchDevice && inputAvailable ? (
            mobileToolsOpen ? (
            <div
              ref={mobileDockRef}
              className={
                'pointer-events-none absolute z-20 flex flex-col gap-1 ' +
                (mobilePos ? '' : 'inset-x-0 bottom-3 items-center')
              }
              style={
                mobilePos ? { left: mobilePos.x, top: mobilePos.y } : undefined
              }
            >
              <div
                data-local-input="true"
                className="pointer-events-auto flex items-center gap-1 rounded-full border border-line bg-surface/90 p-1.5 shadow-lg backdrop-blur"
                onPointerDown={(event) => event.stopPropagation()}
                onPointerUp={(event) => event.stopPropagation()}
              >
                <button
                  type="button"
                  onPointerDown={mobileDockDown}
                  onPointerMove={mobileDockMove}
                  onPointerUp={mobileDockUp}
                  onPointerCancel={mobileDockUp}
                  className="grid h-8 w-7 cursor-move place-items-center text-muted"
                  title={t('session.quickActions')}
                  aria-label={t('session.quickActions')}
                >
                  <GripHorizontal size={13} />
                </button>
                <button
                  type="button"
                  onClick={toggleVirtualMouse}
                  className={
                    'btn h-8 gap-1.5 px-2.5 text-[11px] ' +
                    (virtualMouse ? 'btn-primary' : 'btn-quiet')
                  }
                  aria-pressed={virtualMouse}
                >
                  <MousePointer2 size={14} />
                  {t('session.virtualMouse')}
                </button>
                <button
                  type="button"
                  onClick={openKeyboard}
                  className="btn btn-quiet h-8 gap-1.5 px-2.5 text-[11px]"
                >
                  <Keyboard size={14} />
                  {t('session.virtualKeyboard')}
                </button>
                <button
                  type="button"
                  onClick={() => setMobileToolsOpen(false)}
                  className="grid h-8 w-7 place-items-center text-muted transition hover:text-ink"
                  title={t('session.mobileToolsHide')}
                  aria-label={t('session.mobileToolsHide')}
                >
                  <X size={14} />
                </button>
              </div>
              {virtualMouse ? (
                <div className="pointer-events-none max-w-[70vw] rounded-md border border-line bg-surface/85 px-2 py-1 text-center text-[10px] leading-relaxed text-muted backdrop-blur">
                  {t('session.virtualMouseHint')}
                </div>
              ) : null}
            </div>
            ) : (
              <button
                type="button"
                data-local-input="true"
                onClick={() => setMobileToolsOpen(true)}
                className="pointer-events-auto absolute right-0 top-1/2 z-30 flex h-14 w-9 -translate-y-1/2 items-center justify-center rounded-l-xl border border-r-0 border-line bg-surface/90 text-muted shadow-lg backdrop-blur transition hover:bg-panel hover:text-ink"
                title={t('session.mobileTools')}
                aria-label={t('session.mobileTools')}
                aria-expanded={false}
              >
                <Settings2 size={15} />
              </button>
            )
          ) : null}

          {touchDevice ? (
            <textarea
              ref={keyboardRef}
              aria-hidden
              tabIndex={-1}
              className="pointer-events-none absolute left-0 top-0 h-px w-px resize-none opacity-0"
              autoCapitalize="none"
              autoCorrect="off"
              spellCheck={false}
              onCompositionStart={() => {
                composingRef.current = true
              }}
              onCompositionEnd={(event) => {
                composingRef.current = false
                const value = event.currentTarget.value
                event.currentTarget.value = ''
                if (value) {
                  connection()?.sendInput({ k: 'text', text: value })
                }
              }}
              onInput={(event) => {
                if (composingRef.current) return
                const value = event.currentTarget.value
                event.currentTarget.value = ''
                if (value) {
                  connection()?.sendInput({ k: 'text', text: value })
                }
              }}
            />
          ) : null}
        </div>
      </div>

    </div>
  )
}
