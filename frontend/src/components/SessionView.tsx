import {
  Activity,
  ClipboardCopy,
  ClipboardPaste,
  ExternalLink,
  Gauge,
  GripHorizontal,
  Hand,
  Keyboard,
  LogOut,
  Maximize2,
  Minimize2,
  Monitor,
  MousePointer2,
  Radio,
  Settings2,
  X,
} from 'lucide-react'
import {
  useCallback,
  useEffect,
  useRef,
  useState,
  type PointerEvent as ReactPointerEvent,
} from 'react'
import { useTranslation } from 'react-i18next'
import { codecName } from '../lib/protocol'
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

function formatBandwidth(bytesPerSecond: number): string {
  if (bytesPerSecond >= 1024 * 1024) {
    return `${(bytesPerSecond / (1024 * 1024)).toFixed(1)} MB/s`
  }
  if (bytesPerSecond >= 1024) {
    return `${Math.round(bytesPerSecond / 1024)} KB/s`
  }
  return `${bytesPerSecond} B/s`
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

function Stat({ label, value }: { label: string; value: string }) {
  return (
    <div className="hidden min-w-[62px] rounded-lg border border-line-soft bg-panel px-2 py-1 text-center sm:block">
      <div className="text-[9px] uppercase tracking-[0.14em] text-muted/80">
        {label}
      </div>
      <div className="font-mono text-[11px] text-ink">{value}</div>
    </div>
  )
}

type ResolvedQuality = Exclude<QualityMode, 'auto'>

/// Capture rate and pull interval per preset. The pull interval is the real
/// ceiling: the client asks for the next frame only after the previous one
/// arrived, so a shorter interval is what makes "smooth" actually smoother.
const QUALITY_PROFILES: Record<
  ResolvedQuality,
  { fps: number; pullMs: number }
> = {
  smooth: { fps: 30, pullMs: 33 },
  balanced: { fps: 20, pullMs: 50 },
  quality: { fps: 15, pullMs: 66 },
}

/// Auto may drop to the lossy preset without also raising the frame rate:
/// when the link is already struggling, more frames would make it worse.
const AUTO_PROFILES: Record<
  ResolvedQuality,
  { fps: number; pullMs: number }
> = {
  smooth: { fps: 15, pullMs: 60 },
  balanced: { fps: 20, pullMs: 50 },
  quality: { fps: 15, pullMs: 66 },
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
  const setQualityWatermark = useAppStore((state) => state.setQualityWatermark)
  const qualityMode = useAppStore((state) => state.qualityMode)
  const setQualityMode = useAppStore((state) => state.setQualityMode)
  const setControlEnabled = useAppStore((state) => state.setControlEnabled)
  const closeTab = useAppStore((state) => state.closeTab)
  const createSession = useAppStore((state) => state.createSession)
  const setImmersive = useAppStore((state) => state.setImmersive)

  const welcome = tab.welcome
  const synthetic = welcome?.tiles.synthetic === true
  const inputAvailable = welcome?.input === true && !synthetic
  const clipboardAvailable = welcome?.clipboard === true

  const surfaceRef = useRef<HTMLDivElement>(null)
  const pendingMove = useRef<{ x: number; y: number } | null>(null)
  const moveScheduled = useRef(false)
  const pressedKeys = useRef<Set<string>>(new Set())
  const pressedButtons = useRef<Set<number>>(new Set())
  const [surfaceSize, setSurfaceSize] = useState({ width: 0, height: 0 })
  const [actionError, setActionError] = useState<string | null>(null)
  const touchDevice = touchPrimary()
  const [virtualMouse, setVirtualMouse] = useState(false)
  const [displayMode, setDisplayMode] = useState<DisplayMode>('fit')
  const [elementFullscreen, setElementFullscreen] = useState(false)
  const [shellFullscreen, setShellFullscreen] = useState(false)
  const isFullscreen = elementFullscreen || shellFullscreen
  const [quickOpen, setQuickOpen] = useState(false)
  const [quickPos, setQuickPos] = useState<{ x: number; y: number } | null>(null)
  const quickBarRef = useRef<HTMLDivElement>(null)
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
  const autoModeRef = useRef<QualityMode>('balanced')
  const autoRef = useRef<{
    candidate: ResolvedQuality | null
    streak: number
    lastChange: number
  }>({ candidate: null, streak: 0, lastChange: 0 })
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
  const activeProfile =
    qualityMode === 'auto'
      ? AUTO_PROFILES[resolvedMode]
      : QUALITY_PROFILES[resolvedMode]
  const pullMs = activeProfile.pullMs

  const connection = useCallback(
    () => sessionConnection(tab.id),
    [tab.id],
  )

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
  const requestAccessFromError = useCallback(() => {
    createSession(tab.host, tab.port, '', tab.label, false, true)
    closeTab(tab.id)
  }, [closeTab, createSession, tab.host, tab.label, tab.port, tab.id])
  const reconnect = useCallback(() => {
    connection()?.connect(tab.host, tab.port, tab.token, tab.token === '')
  }, [connection, tab.host, tab.port, tab.token])

  // A background tab is throttled and may miss frames; resync as soon as it
  // becomes visible again instead of waiting for the next pull.
  useEffect(() => {
    const onVisible = () => {
      if (document.visibilityState !== 'visible') return
      if (tab.phase !== 'live') return
      connection()?.requestKeyframe()
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
    const update = () => setElementFullscreen(document.fullscreenElement !== null)
    document.addEventListener('fullscreenchange', update)
    update()
    return () => document.removeEventListener('fullscreenchange', update)
  }, [])

  // The expanded quick bar collapses when focus or the pointer moves outside.
  useEffect(() => {
    if (!quickOpen) return
    const onPointerDown = (event: PointerEvent) => {
      if (!quickBarRef.current?.contains(event.target as Node)) {
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
  }, [quickOpen])

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

  // Keep the auto controller's view of the link fresh without restarting its
  // interval on every stats patch.
  useEffect(() => {
    statsRef.current = tab.stats
  }, [tab.stats])

  // Push the active preset after a short settle delay. Both the encoding mode
  // and the capture rate are host-global, like the old capture-fps message.
  useEffect(() => {
    if (tab.phase !== 'live') return
    const timer = window.setTimeout(() => {
      connection()?.setQuality(resolvedMode, activeProfile.fps)
    }, 400)
    return () => window.clearTimeout(timer)
  }, [
    tab.phase,
    tab.id,
    connection,
    resolvedMode,
    activeProfile.fps,
  ])

  // Auto: pick a preset from RTT, the completed pull rate and throughput. Two
  // consecutive agreeing samples and a 3 s dwell keep it from oscillating.
  useEffect(() => {
    if (tab.phase !== 'live' || qualityMode !== 'auto') {
      // Remember the preset we left so re-entering auto starts fresh.
      autoModeRef.current = qualityMode
      return
    }
    if (autoModeRef.current !== qualityMode) {
      autoModeRef.current = qualityMode
      autoRef.current = { candidate: null, streak: 0, lastChange: 0 }
    }
    const timer = window.setInterval(() => {
      // A hidden window does not pull frames, so it must not drive the
      // host-global preset from a stale picture of the link.
      if (document.visibilityState === 'hidden') return
      const stats = statsRef.current
      const latency = stats.latencyMs
      const wireFps = stats.wireFps
      const bandwidth = stats.bytesPerSec

      // Throughput on its own is not a problem: a busy screen on a healthy
      // link can move a lot of bytes. Latency and the rate at which pull
      // requests complete are the signals that say the link is falling behind.
      let candidate: ResolvedQuality = 'balanced'
      if (
        (latency !== null && latency > 220) ||
        (wireFps > 0 && wireFps < 7)
      ) {
        candidate = 'smooth'
      } else if (
        latency !== null &&
        latency < 90 &&
        wireFps >= 12 &&
        bandwidth < 16 * 1024 * 1024
      ) {
        candidate = 'quality'
      }

      const state = autoRef.current
      if (candidate === resolvedMode) {
        state.candidate = null
        state.streak = 0
        return
      }
      if (state.candidate === candidate) {
        state.streak += 1
      } else {
        state.candidate = candidate
        state.streak = 1
      }
      const now = performance.now()
      if (state.streak >= 2 && now - state.lastChange >= 3000) {
        state.lastChange = now
        state.candidate = null
        state.streak = 0
        setAutoTier(candidate)
      }
    }, 1000)
    return () => window.clearInterval(timer)
  }, [tab.phase, tab.id, qualityMode, resolvedMode])

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

  const toggleFullscreen = () => {
    const next = !isFullscreen
    if (shellInfo.enabled) {
      // The native window is the fullscreen surface. Requesting HTML element
      // fullscreen as well made WebKitGTK and the shell fight over the window
      // state, which left the canvas offset after the transition.
      windowAction('fullscreen')
      setShellFullscreen(next)
      setImmersive(next)
    } else {
      // A browser has no native window to toggle; the fullscreen element also
      // hides the app chrome.
      if (document.fullscreenElement) {
        void document.exitFullscreen()
      } else if (next) {
        void sessionRootRef.current?.requestFullscreen().catch(() => undefined)
      }
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
  }

  // Leaving the session (tab closed, disconnect) must not leave the window in
  // fullscreen or the chrome hidden.
  useEffect(() => {
    return () => setImmersive(false)
  }, [setImmersive])

  const changeDisplayMode = (mode: DisplayMode) => {
    setDisplayMode(mode)
    setQuickOpen(false)
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
      event.preventDefault()
      keys.delete(code)
      connection()?.sendInput({ k: 'key', code, down: false })
    }
    window.addEventListener('keydown', down)
    window.addEventListener('keyup', up)
    return () => {
      window.removeEventListener('keydown', down)
      window.removeEventListener('keyup', up)
      for (const code of keys) {
        connection()?.sendInput({ k: 'key', code, down: false })
      }
      keys.clear()
      for (const button of buttons) {
        connection()?.sendInput({ k: 'button', button, down: false })
      }
      buttons.clear()
      setControlEnabled(tab.id, false)
    }
  }, [tab.controlEnabled, tab.id, setControlEnabled, connection])

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
  const transportLabel =
    tab.welcome?.transport === 'websocket'
      ? 'WS'
      : (tab.welcome?.transport ?? '—')
  // Follow the frame actually on screen; the welcome codec is only a snapshot
  // from connect time and goes stale when the preset changes.
  const codecLabel = tab.frame
    ? codecName(tab.frame.codec)
    : (tab.welcome?.tiles.codec ?? '')

  return (
    <div ref={sessionRootRef} className="flex min-h-0 flex-1 flex-col">
      <div className="flex flex-wrap items-center gap-2 border-b border-line-soft px-4 py-2.5">
        <div className="min-w-0">
          <div className="flex items-center gap-2">
            <span
              className={
                'h-1.5 w-1.5 rounded-full ' +
                (tab.phase === 'live'
                  ? 'bg-accent'
                  : tab.phase === 'connecting'
                    ? 'animate-pulse bg-warn'
                    : 'bg-danger')
              }
            />
            <div className="truncate text-sm font-medium">{tab.label}</div>
          </div>
          <div className="mt-0.5 truncate font-mono text-[11px] text-muted">
            {tab.host}:{tab.port}
            {welcome?.device_id ? ` · ${welcome.device_id}` : ''}
          </div>
        </div>

        <div className="ml-auto flex items-center gap-1.5">
          <div className="flex items-center gap-1.5">
            <Stat label={t('stats.fps')} value={String(tab.stats.fps)} />
            <Stat label={t('stats.latency')} value={latency} />
            <Stat
              label={t('stats.bandwidth')}
              value={formatBandwidth(tab.stats.bytesPerSec)}
            />
          </div>

          {clipboardAvailable ? (
            <>
              <button
                type="button"
                onClick={copyFromRemote}
                className="btn btn-quiet h-8 w-8 !px-0"
                title={t('session.clipboardGet')}
                aria-label={t('session.clipboardGet')}
              >
                <ClipboardCopy size={14} />
              </button>
              <button
                type="button"
                onClick={sendToRemote}
                className="btn btn-quiet h-8 w-8 !px-0"
                title={t('session.clipboardSet')}
                aria-label={t('session.clipboardSet')}
              >
                <ClipboardPaste size={14} />
              </button>
            </>
          ) : null}

          <button
            type="button"
            onClick={() => setQualityWatermark(!qualityWatermark)}
            className={
              'btn h-8 w-8 !px-0 ' +
              (qualityWatermark ? 'text-accent' : 'btn-quiet')
            }
            title={t('settings.watermark')}
            aria-label={t('settings.watermark')}
          >
            <Activity size={14} />
          </button>

          {!popout && localMode ? (
            <button
              type="button"
              onClick={openPopout}
              className="btn btn-quiet h-8 w-8 !px-0"
              title={t('session.popOut')}
              aria-label={t('session.popOut')}
            >
              <ExternalLink size={14} />
            </button>
          ) : null}

          {inputAvailable ? (
            <button
              type="button"
              onClick={() => {
                const next = !tab.controlEnabled
                setControlEnabled(tab.id, next)
                if (!next) setVirtualMouse(false)
              }}
              className={
                tab.controlEnabled
                  ? 'btn border-accent/50 bg-accent-soft text-accent'
                  : 'btn'
              }
            >
              {tab.controlEnabled ? <Hand size={14} /> : <MousePointer2 size={14} />}
              <span className="hidden md:inline">
                {tab.controlEnabled
                  ? t('session.stopControl')
                  : t('session.startControl')}
              </span>
            </button>
          ) : (
            <span className="chip">{t('session.inputUnavailable')}</span>
          )}

          {inputAvailable ? (
            <button
              type="button"
              onClick={() => connection()?.sendInput({ k: 'sas' })}
              className="btn"
              title={t('session.ctrlAltDel')}
              aria-label={t('session.ctrlAltDel')}
            >
              <Keyboard size={14} />
              <span className="hidden md:inline">
                {t('session.ctrlAltDel')}
              </span>
            </button>
          ) : null}

          {popout ? (
            <button
              type="button"
              onClick={() => window.close()}
              className="btn btn-quiet h-8 w-8 !px-0"
              title={t('session.closeWindow')}
              aria-label={t('session.closeWindow')}
            >
              <X size={15} />
            </button>
          ) : (
            <button
              type="button"
              onClick={() => closeTab(tab.id)}
              className="btn btn-quiet h-8 w-8 !px-0 hover:!border-danger/50 hover:!text-danger"
              title={t('session.disconnect')}
              aria-label={t('session.disconnect')}
            >
              <LogOut size={14} />
            </button>
          )}
        </div>
      </div>

      {tab.error || actionError ? (
        <div className="flex flex-wrap items-center gap-2 border-b border-danger/30 bg-danger/10 px-4 py-2 text-[12px] text-danger">
          <span className="min-w-0 flex-1">
            {tab.error
              ? String(t(`error.${tab.error}`, { defaultValue: tab.error }))
              : actionError}
          </span>
          {tab.error ? (
            <button
              type="button"
              onClick={reconnect}
              className="btn h-7 px-2 text-[11px]"
            >
              {t('session.reconnect')}
            </button>
          ) : null}
          {tab.error && /token|unauth|auth|invalid/i.test(tab.error) ? (
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

      <div className="flex min-h-0 flex-1 items-center justify-center p-3 sm:p-4">
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
          onContextMenu={(event) => {
            if (tab.controlEnabled) event.preventDefault()
          }}
          className={
            'relative h-full w-full touch-none select-none rounded-xl border bg-black/70 outline-none transition ' +
            (displayMode === 'actual'
              ? 'overflow-auto '
              : 'flex items-center justify-center overflow-hidden ') +
            (tab.controlEnabled ? 'border-accent/45' : 'border-line')
          }
        >
          {tab.frame ? (
            <div
              className={
                displayMode === 'actual'
                  ? 'relative'
                  : 'relative h-full w-full'
              }
              style={
                displayMode === 'actual'
                  ? {
                      width: tab.frame.screenW,
                      height: tab.frame.screenH,
                    }
                  : undefined
              }
            >
              <TileCanvas frame={tab.frame} mode={displayMode} />
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

          {quickOpen ? (
            <div
              ref={quickBarRef}
              className="pointer-events-auto absolute left-1/2 top-3 z-30 flex max-w-[94vw] -translate-x-1/2 items-center gap-1 overflow-x-auto rounded-full border border-line bg-surface/95 p-1 shadow-lg backdrop-blur"
              onPointerDown={(event) => event.stopPropagation()}
              onPointerUp={(event) => event.stopPropagation()}
              onBlur={(event) => {
                if (
                  !event.currentTarget.contains(event.relatedTarget as Node)
                ) {
                  setQuickOpen(false)
                }
              }}
            >
              <button
                type="button"
                onClick={() => setQuickOpen(false)}
                className="btn btn-quiet h-7 w-7 shrink-0 !px-0"
                title={t('session.quickActions')}
                aria-label={t('session.quickActions')}
              >
                <Settings2 size={14} />
              </button>
              <button
                type="button"
                onClick={toggleFullscreen}
                className="btn btn-quiet h-7 gap-1 px-2 text-[11px]"
                title={
                  isFullscreen
                    ? t('session.exitFullscreen')
                    : t('session.fullscreen')
                }
              >
                {isFullscreen ? (
                  <Minimize2 size={13} />
                ) : (
                  <Maximize2 size={13} />
                )}
                <span className="hidden sm:inline">
                  {isFullscreen
                    ? t('session.exitFullscreen')
                    : t('session.fullscreen')}
                </span>
              </button>
              <span className="mx-0.5 h-4 w-px bg-line" />
              {(['fit', 'actual', 'fill'] as DisplayMode[]).map((mode) => (
                <button
                  key={mode}
                  type="button"
                  onClick={() => changeDisplayMode(mode)}
                  className={
                    'btn h-7 px-2 text-[11px] ' +
                    (displayMode === mode ? 'btn-primary' : 'btn-quiet')
                  }
                >
                  {t(`session.${mode}`)}
                </button>
              ))}
              <span className="mx-0.5 h-4 w-px bg-line" />
              <label
                className="flex items-center gap-1 rounded-md border border-line bg-panel px-1.5 text-muted"
                title={t('session.quality')}
              >
                <Gauge size={12} />
                <select
                  value={qualityMode}
                  onChange={(event) =>
                    setQualityMode(event.target.value as QualityMode)
                  }
                  aria-label={t('session.quality')}
                  className="h-7 bg-transparent text-[11px] text-ink outline-none"
                >
                  <option value="smooth">{t('session.qualitySmooth')}</option>
                  <option value="balanced">{t('session.qualityBalanced')}</option>
                  <option value="quality">{t('session.qualityHigh')}</option>
                  <option value="auto">{t('session.qualityAuto')}</option>
                </select>
              </label>
              <button
                type="button"
                onClick={() => setQualityWatermark(!qualityWatermark)}
                className={
                  'btn h-7 w-7 !px-0 ' +
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
                  className="flex items-center gap-1 rounded-md border border-line bg-panel px-1.5 text-muted"
                  title={t('session.display')}
                >
                  <Monitor size={12} />
                  <select
                    value={tab.displayIndex}
                    onChange={(event) =>
                      connection()?.setDisplay(Number(event.target.value))
                    }
                    aria-label={t('session.display')}
                    className="h-7 max-w-[8rem] bg-transparent text-[11px] text-ink outline-none"
                  >
                    {displays.map((display) => (
                      <option key={display.index} value={display.index}>
                        {display.name || `Display ${display.index + 1}`}
                      </option>
                    ))}
                  </select>
                </label>
              ) : null}
              {welcome && !welcome.tiles.synthetic ? (
                <label
                  className="flex items-center gap-1 rounded-md border border-line bg-panel px-1.5 text-muted"
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
                    className="h-7 bg-transparent text-[11px] text-ink outline-none"
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
              <span className="chip" title={t('session.transport')}>
                {transportLabel}
              </span>
            </div>
          ) : (
            <button
              type="button"
              onPointerDown={quickHandleDown}
              onPointerMove={quickHandleMove}
              onPointerUp={quickHandleUp}
              onPointerCancel={quickHandleUp}
              onClick={toggleQuickOpen}
              className="pointer-events-auto absolute z-30 grid h-9 w-9 cursor-move place-items-center rounded-full border border-line bg-surface/90 text-muted shadow-lg backdrop-blur hover:text-ink"
              style={{ left: quickHandlePos.x, top: quickHandlePos.y }}
              title={t('session.quickActions')}
              aria-label={t('session.quickActions')}
            >
              <Settings2 size={15} />
            </button>
          )}

          <div className="pointer-events-none absolute left-3 top-3 flex flex-wrap gap-1.5">
            {qualityWatermark && welcome ? (
              <span className="rounded-md border border-line bg-surface/80 px-2 py-1 font-mono text-[10px] text-muted backdrop-blur">
                {welcome.tiles.screen_w}×{welcome.tiles.screen_h}
              </span>
            ) : null}
            {qualityWatermark && welcome ? (
              <span className="rounded-md border border-line bg-surface/80 px-2 py-1 font-mono text-[10px] uppercase text-muted backdrop-blur">
                {codecLabel}
              </span>
            ) : null}
            {syntheticLabel ? (
              <span className="rounded-md border border-warn/40 bg-warn/10 px-2 py-1 font-mono text-[10px] text-warn backdrop-blur">
                {syntheticLabel}
              </span>
            ) : null}
            {tab.controlEnabled ? (
              <span className="rounded-md border border-accent/50 bg-accent-soft px-2 py-1 font-mono text-[10px] text-accent backdrop-blur">
                {t('session.controlOn')}
              </span>
            ) : null}
          </div>

          {qualityWatermark ? (
            <div className="pointer-events-none absolute bottom-3 right-3 flex items-center gap-2 rounded-md border border-line bg-surface/75 px-2.5 py-1 font-mono text-[10px] text-muted backdrop-blur">
              <span className="text-accent">{tab.stats.fps} fps</span>
              <span>{latency}</span>
              <span>{formatBandwidth(tab.stats.bytesPerSec)}</span>
              <span className="text-muted/60">{codecLabel}</span>
            </div>
          ) : null}

          {touchDevice && inputAvailable ? (
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
              </div>
              {virtualMouse ? (
                <div className="pointer-events-none max-w-[70vw] rounded-md border border-line bg-surface/85 px-2 py-1 text-center text-[10px] leading-relaxed text-muted backdrop-blur">
                  {t('session.virtualMouseHint')}
                </div>
              ) : null}
            </div>
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

      <div className="flex flex-wrap items-center justify-between gap-2 border-t border-line-soft px-4 py-2 text-[11px] text-muted/85">
        <span>{synthetic ? t('session.synthetic') : t('session.direct')}</span>
        <span>
          {tab.controlEnabled
            ? t('session.controlOnHint')
            : inputAvailable
              ? t('session.controlHint')
              : t('session.inputUnavailable')}
        </span>
      </div>
    </div>
  )
}
