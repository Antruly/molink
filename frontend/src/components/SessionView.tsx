import { LogOut, Radio } from 'lucide-react'
import { useEffect } from 'react'
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
  const disconnect = useSessionStore((state) => state.disconnect)

  // Request the next frame only when the previous one has arrived, so a slow
  // link degrades to a lower frame rate instead of a growing request queue.
  useEffect(() => {
    if (phase !== 'live') return
    const timer = window.setInterval(() => {
      client.requestFrame()
    }, 60)
    return () => window.clearInterval(timer)
  }, [phase])

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

      <div className="flex min-h-0 flex-1 items-center justify-center p-3 sm:p-5">
        <div className="relative flex h-full w-full items-center justify-center overflow-hidden rounded-xl border border-line bg-black/60">
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
          </div>
        </div>
      </div>

      <div className="flex flex-wrap items-center justify-between gap-2 border-t border-line-soft px-5 py-2.5 text-[11px] text-fog/80 sm:px-8">
        <span className="flex items-center gap-1.5 text-amber/90">
          <span className="h-1 w-1 rounded-full bg-amber" />
          {welcome?.tiles.synthetic ? t('session.synthetic') : null}
        </span>
        <span>{t('session.previewOnly')}</span>
      </div>
    </div>
  )
}
