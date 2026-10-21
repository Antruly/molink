import { Hand, LogOut, MousePointer2, Radio } from 'lucide-react'
import { useEffect, useRef, type PointerEvent as ReactPointerEvent } from 'react'
import { useTranslation } from 'react-i18next'
import type { Stats } from '../store/session'
import { client, useSessionStore } from '../store/session'
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

function Stat({ label, value }: { label: string; value: string }) {
  return (
    <div className="hidden min-w-[68px] rounded-md border border-line-soft bg-panel px-2.5 py-1.5 text-center sm:block">
      <div className="text-[10px] uppercase tracking-[0.14em] text-fog/70">
        {label}
      </div>
      <div className="font-mono text-xs text-mist">{value}</div>
    </div>
  )
}

function StatsRow({ stats }: { stats: Stats }) {
  const { t } = useTranslation()
  return (
    <>
      <Stat label={t('stats.fps')} value={String(stats.fps)} />
      <Stat
        label={t('stats.latency')}
        value={stats.latencyMs === null ? '—' : `${Math.round(stats.latencyMs)} ms`}
      />
      <Stat
        label={t('stats.bandwidth')}
        value={formatBandwidth(stats.bytesPerSec)}
      />
    </>
  )
}

export function SessionView() {
  const { t } = useTranslation()
  const phase = useSessionStore((state) => state.phase)
  const welcome = useSessionStore((state) => state.welcome)
  const frame = useSessionStore((state) => state.frame)
  const stats = useSessionStore((state) => state.stats)
  const error = useSessionStore((state) => state.error)
  const controlEnabled = useSessionStore((state) => state.controlEnabled)
  const setControlEnabled = useSessionStore((state) => state.setControlEnabled)
  const disconnect = useSessionStore((state) => state.disconnect)

  const synthetic = welcome?.tiles.synthetic === true
  // Blind input into a real desktop is not useful and can be destructive:
  // control stays off whenever the picture is a test pattern.
  const inputAvailable = welcome?.input === true && !synthetic
  const surfaceRef = useRef<HTMLDivElement>(null)
  const pendingMove = useRef<{ x: number; y: number } | null>(null)
  const moveScheduled = useRef(false)
  const pressedKeys = useRef<Set<string>>(new Set())
  const pressedButtons = useRef<Set<number>>(new Set())

  // Request the next frame only when the previous one has arrived, so a slow
  // link degrades to a lower frame rate instead of a growing request queue.
  useEffect(() => {
    if (phase !== 'live') return
    const timer = window.setInterval(() => {
      client.requestFrame()
    }, 60)
    return () => window.clearInterval(timer)
  }, [phase])

  const normalize = (clientX: number, clientY: number) => {
    const element = surfaceRef.current
    if (!element || !welcome) return null
    const screenW = welcome.tiles.screen_w
    const screenH = welcome.tiles.screen_h
    if (screenW <= 0 || screenH <= 0) return null

    const rect = element.getBoundingClientRect()
    // The canvas uses object-fit: contain, so the image may be letterboxed.
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

  const onPointerDown = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (!controlEnabled || !inputAvailable) return
    const point = normalize(event.clientX, event.clientY)
    if (!point) return
    event.preventDefault()
    event.currentTarget.focus()
    event.currentTarget.setPointerCapture(event.pointerId)
    client.sendInput({ k: 'move', ...point })
    if (event.button <= 2) {
      const button = event.button + 1
      pressedButtons.current.add(button)
      client.sendInput({ k: 'button', button, down: true })
    }
  }

  const onPointerUp = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (!controlEnabled || !inputAvailable) return
    if (event.currentTarget.hasPointerCapture(event.pointerId)) {
      event.currentTarget.releasePointerCapture(event.pointerId)
    }
    if (event.button <= 2) {
      const button = event.button + 1
      pressedButtons.current.delete(button)
      client.sendInput({ k: 'button', button, down: false })
    }
  }

  const onPointerMove = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (!controlEnabled || !inputAvailable) return
    const point = normalize(event.clientX, event.clientY)
    if (!point) return
    pendingMove.current = point
    if (moveScheduled.current) return
    moveScheduled.current = true
    window.requestAnimationFrame(() => {
      moveScheduled.current = false
      const next = pendingMove.current
      if (next) client.sendInput({ k: 'move', ...next })
    })
  }

  // A native non-passive wheel listener: React's synthetic one cannot
  // preventDefault, and the page should not scroll while controlling.
  useEffect(() => {
    const element = surfaceRef.current
    if (!element || !controlEnabled || !inputAvailable) return
    const onWheel = (event: WheelEvent) => {
      event.preventDefault()
      const dy = event.deltaY === 0 ? 0 : event.deltaY > 0 ? 1 : -1
      const dx = event.deltaX === 0 ? 0 : event.deltaX > 0 ? 1 : -1
      if (dx !== 0 || dy !== 0) client.sendInput({ k: 'wheel', dx, dy })
    }
    element.addEventListener('wheel', onWheel, { passive: false })
    return () => element.removeEventListener('wheel', onWheel)
  }, [controlEnabled, inputAvailable])

  // Keyboard capture lives at the window level, including the cleanup that
  // releases held keys when control is switched off.
  useEffect(() => {
    if (!controlEnabled) return
    const keys = pressedKeys.current
    const buttons = pressedButtons.current
    const down = (event: KeyboardEvent) => {
      if (event.repeat) return
      event.preventDefault()
      keys.add(event.code)
      client.sendInput({ k: 'key', code: event.code, down: true })
    }
    const up = (event: KeyboardEvent) => {
      event.preventDefault()
      keys.delete(event.code)
      client.sendInput({ k: 'key', code: event.code, down: false })
    }
    window.addEventListener('keydown', down)
    window.addEventListener('keyup', up)
    return () => {
      window.removeEventListener('keydown', down)
      window.removeEventListener('keyup', up)
      for (const code of keys) {
        client.sendInput({ k: 'key', code, down: false })
      }
      keys.clear()
      for (const button of buttons) {
        client.sendInput({ k: 'button', button, down: false })
      }
      buttons.clear()
    }
  }, [controlEnabled])

  return (
    <div className="flex min-h-0 flex-1 flex-col">
      <div className="flex flex-wrap items-center justify-between gap-3 border-b border-line-soft px-5 py-3 sm:px-8">
        <div className="flex items-center gap-3">
          <span className="relative flex h-2 w-2">
            <span className="absolute inline-flex h-full w-full animate-ping rounded-full bg-signal opacity-60" />
            <span className="relative inline-flex h-2 w-2 rounded-full bg-signal" />
          </span>
          <div>
            <div className="font-mono text-sm tracking-[0.18em] text-mist">
              {welcome?.session ?? '—'}
            </div>
            <div className="text-[11px] text-fog">
              {welcome?.service ?? 'moLink Core'}
              {welcome?.version ? ` · v${welcome.version}` : ''}
            </div>
          </div>
        </div>

        <div className="flex items-center gap-2">
          <StatsRow stats={stats} />
          {inputAvailable ? (
            <button
              type="button"
              onClick={() => setControlEnabled(!controlEnabled)}
              className={
                controlEnabled
                  ? 'flex items-center gap-1.5 rounded-md border border-signal/60 bg-signal/15 px-2.5 py-1.5 text-[11px] text-signal transition'
                  : 'flex items-center gap-1.5 rounded-md border border-line bg-panel px-2.5 py-1.5 text-[11px] text-fog transition hover:border-signal/50 hover:text-mist'
              }
            >
              {controlEnabled ? <Hand size={13} /> : <MousePointer2 size={13} />}
              {controlEnabled
                ? t('session.stopControl')
                : t('session.startControl')}
            </button>
          ) : null}
          <button
            type="button"
            onClick={disconnect}
            className="flex items-center gap-1.5 rounded-md border border-line bg-panel px-2.5 py-1.5 text-[11px] text-fog transition hover:border-alert/50 hover:text-alert"
          >
            <LogOut size={13} />
            {t('session.disconnect')}
          </button>
        </div>
      </div>

      {error ? (
        <div className="border-b border-alert/30 bg-alert/10 px-5 py-2 text-xs text-alert sm:px-8">
          {error}
        </div>
      ) : null}

      <div className="flex min-h-0 flex-1 items-center justify-center p-3 sm:p-5">
        <div
          ref={surfaceRef}
          tabIndex={-1}
          onPointerDown={onPointerDown}
          onPointerUp={onPointerUp}
          onPointerMove={onPointerMove}
          onContextMenu={(event) => {
            if (controlEnabled) event.preventDefault()
          }}
          className={
            'relative flex h-full w-full touch-none select-none items-center justify-center overflow-hidden rounded-xl border bg-black/60 outline-none ' +
            (controlEnabled ? 'border-signal/40' : 'border-line')
          }
        >
          {frame ? (
            <TileCanvas frame={frame} />
          ) : (
            <div className="flex items-center gap-2 text-sm text-fog">
              <Radio size={15} className="animate-pulse text-signal" />
              {t('session.waiting')}
            </div>
          )}

          <div className="pointer-events-none absolute left-3 top-3 flex flex-wrap gap-2">
            {welcome ? (
              <span className="rounded border border-line bg-ink/80 px-2 py-1 font-mono text-[10px] text-fog">
                {welcome.tiles.screen_w}×{welcome.tiles.screen_h}
              </span>
            ) : null}
            {welcome ? (
              <span className="rounded border border-line bg-ink/80 px-2 py-1 font-mono text-[10px] uppercase text-fog">
                {welcome.tiles.codec}
              </span>
            ) : null}
            <span className="rounded border border-line bg-ink/80 px-2 py-1 font-mono text-[10px] text-fog">
              {t('session.frames')} {stats.frames}
            </span>
            {controlEnabled ? (
              <span className="rounded border border-signal/50 bg-signal/15 px-2 py-1 font-mono text-[10px] text-signal">
                {t('session.controlOn')}
              </span>
            ) : null}
          </div>
        </div>
      </div>

      <div className="flex flex-wrap items-center justify-between gap-2 border-t border-line-soft px-5 py-2.5 text-[11px] text-fog/80 sm:px-8">
        <span className="flex items-center gap-1.5 text-amber/90">
          <span className="h-1 w-1 rounded-full bg-amber" />
          {welcome?.tiles.synthetic ? t('session.synthetic') : null}
        </span>
        <span>
          {controlEnabled
            ? t('session.controlOnHint')
            : inputAvailable
              ? t('session.controlHint')
              : synthetic
                ? t('session.controlSynthetic')
                : t('session.inputUnavailable')}
        </span>
      </div>
    </div>
  )
}
