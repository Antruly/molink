import {
  Activity,
  ClipboardCopy,
  ClipboardPaste,
  ExternalLink,
  Hand,
  Keyboard,
  LogOut,
  Monitor,
  MousePointer2,
  Radio,
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
import { openSessionWindow } from '../lib/windows'
import {
  sessionConnection,
  useAppStore,
  type SessionTab,
} from '../store/app'
import { RemoteCursor } from './RemoteCursor'
import { TileCanvas } from './TileCanvas'

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
  const setControlEnabled = useAppStore((state) => state.setControlEnabled)
  const closeTab = useAppStore((state) => state.closeTab)

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

  const connection = useCallback(
    () => sessionConnection(tab.id),
    [tab.id],
  )

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
    }, 60)
    return () => window.clearInterval(timer)
  }, [tab.phase, tab.id, connection])

  // Adaptive capture rate. The core captures at the requested rate and the
  // client pulls the newest deltas; lowering it is the safe lever when RTT or
  // throughput says the link is struggling. Hysteresis avoids oscillation.
  const adaptiveFps = useRef(15)
  const adaptiveTimer = useRef<number | null>(null)
  useEffect(() => {
    if (tab.phase !== 'live') return
    const latency = tab.stats.latencyMs
    const bandwidth = tab.stats.bytesPerSec
    let target = 15
    if ((latency !== null && latency > 250) || bandwidth > 40 * 1024 * 1024) {
      target = 5
    } else if (
      (latency !== null && latency > 120) ||
      bandwidth > 20 * 1024 * 1024
    ) {
      target = 10
    }
    if (target === adaptiveFps.current) return
    adaptiveFps.current = target
    if (adaptiveTimer.current !== null) {
      window.clearTimeout(adaptiveTimer.current)
    }
    adaptiveTimer.current = window.setTimeout(() => {
      adaptiveTimer.current = null
      connection()?.setCaptureFps(target)
    }, 800)
  }, [tab.phase, tab.stats.latencyMs, tab.stats.bytesPerSec, connection, tab.id])

  useEffect(
    () => () => {
      if (adaptiveTimer.current !== null) {
        window.clearTimeout(adaptiveTimer.current)
      }
    },
    [],
  )

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
    if (!element || !welcome) return null
    const screenW = welcome.tiles.screen_w
    const screenH = welcome.tiles.screen_h
    if (screenW <= 0 || screenH <= 0) return null
    const rect = element.getBoundingClientRect()
    const scale = Math.min(rect.width / screenW, rect.height / screenH)
    const drawnWidth = screenW * scale
    const drawnHeight = screenH * scale
    const offsetX = (rect.width - drawnWidth) / 2
    const offsetY = (rect.height - drawnHeight) / 2
    const nx = (clientX - rect.left - offsetX) / drawnWidth
    const ny = (clientY - rect.top - offsetY) / drawnHeight
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
      event.preventDefault()
      const dy = event.deltaY === 0 ? 0 : event.deltaY > 0 ? 1 : -1
      const dx = event.deltaX === 0 ? 0 : event.deltaX > 0 ? 1 : -1
      if (dx !== 0 || dy !== 0) connection()?.sendInput({ k: 'wheel', dx, dy })
    }
    element.addEventListener('wheel', onWheel, { passive: false })
    return () => element.removeEventListener('wheel', onWheel)
  }, [tab.controlEnabled, inputAvailable, tab.id, connection])

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

  return (
    <div className="flex min-h-0 flex-1 flex-col">
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
          {(tab.welcome?.displays?.length ?? 0) > 1 ? (
            <label
              className="flex items-center gap-1 rounded-md border border-line bg-panel px-1.5 text-muted"
              title={t('session.display')}
            >
              <Monitor size={13} />
              <select
                value={tab.displayIndex}
                onChange={(event) =>
                  connection()?.setDisplay(Number(event.target.value))
                }
                aria-label={t('session.display')}
                className="h-8 max-w-[9rem] bg-transparent pr-1 text-[11px] text-ink outline-none"
              >
                {tab.welcome?.displays?.map((display) => (
                  <option key={display.index} value={display.index}>
                    {display.name || `Display ${display.index + 1}`}
                  </option>
                ))}
              </select>
            </label>
          ) : null}
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
        <div className="border-b border-danger/30 bg-danger/10 px-4 py-2 text-[12px] text-danger">
          {tab.error
            ? String(t(`error.${tab.error}`, { defaultValue: tab.error }))
            : actionError}
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
          onContextMenu={(event) => {
            if (tab.controlEnabled) event.preventDefault()
          }}
          className={
            'relative flex h-full w-full touch-none select-none items-center justify-center overflow-hidden rounded-xl border bg-black/70 outline-none transition ' +
            (tab.controlEnabled ? 'border-accent/45' : 'border-line')
          }
        >
          {tab.frame ? (
            <TileCanvas frame={tab.frame} />
          ) : (
            <div className="flex items-center gap-2 text-sm text-muted">
              <Radio size={15} className="animate-pulse text-accent" />
              {tab.phase === 'connecting'
                ? t('session.connecting')
                : t('session.waiting')}
            </div>
          )}

          {(!tab.controlEnabled || virtualMouse) && !synthetic ? (
            <RemoteCursor
              shape={tab.cursorShape}
              position={tab.cursorPosition}
              welcome={welcome}
              viewWidth={surfaceSize.width}
              viewHeight={surfaceSize.height}
              screenWidth={tab.frame?.screenW}
              screenHeight={tab.frame?.screenH}
            />
          ) : null}

          <div className="pointer-events-none absolute left-3 top-3 flex flex-wrap gap-1.5">
            {welcome ? (
              <span className="rounded-md border border-line bg-surface/80 px-2 py-1 font-mono text-[10px] text-muted backdrop-blur">
                {welcome.tiles.screen_w}×{welcome.tiles.screen_h}
              </span>
            ) : null}
            {welcome ? (
              <span className="rounded-md border border-line bg-surface/80 px-2 py-1 font-mono text-[10px] uppercase text-muted backdrop-blur">
                {welcome.tiles.codec}
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
              <span className="text-muted/60">{welcome?.tiles.codec ?? ''}</span>
            </div>
          ) : null}

          {touchDevice && inputAvailable ? (
            <div className="pointer-events-none absolute inset-x-0 bottom-3 z-20 flex flex-col items-center gap-1">
              <div
                className="pointer-events-auto flex items-center gap-1 rounded-full border border-line bg-surface/90 p-1.5 shadow-lg backdrop-blur"
                onPointerDown={(event) => event.stopPropagation()}
                onPointerUp={(event) => event.stopPropagation()}
              >
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
                <div className="pointer-events-none max-w-[92%] rounded-md border border-line bg-surface/85 px-2 py-1 text-center text-[10px] leading-relaxed text-muted backdrop-blur">
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
