import { useEffect, useRef, useState } from 'react'
import { createPortal } from 'react-dom'
import { useTranslation } from 'react-i18next'
import { Gauge, X } from 'lucide-react'
import {
  SESSION_PRESETS,
  chromaLabel,
  isExplicitStreamCodec,
  presetPlan,
  type PresetApplied,
  type SessionPresetId,
} from '../lib/session-presets'

/// The toolbar's 画质 group. This is the viewer's picture tier: fixed presets
/// (plus a separate 自动), stored per host:port. Clicking one takes effect
/// immediately and is persisted by the caller; the small line underneath is
/// what the host reported it actually applied (fps / codec / chroma).
export function QualityPanel({
  preset,
  applied,
  codecPreference,
  onPreset,
}: {
  preset: SessionPresetId
  applied?: PresetApplied
  /// The host's global encoder preference. An explicit one is 4:2:0, so a
  /// truecolour tier is really running at reduced chroma.
  codecPreference?: string
  onPreset: (preset: SessionPresetId) => void
}) {
  const { t } = useTranslation()
  const [open, setOpen] = useState(false)
  const [spot, setSpot] = useState<{ left: number; top: number } | null>(null)
  const button = useRef<HTMLButtonElement>(null)
  const holder = useRef<HTMLDivElement>(null)
  const surface = useRef<HTMLDivElement>(null)

  useEffect(() => {
    if (!open) return
    const close = (event: Event) => {
      const target = event.target as Node
      if (holder.current?.contains(target)) return
      if (surface.current?.contains(target)) return
      setOpen(false)
    }
    document.addEventListener('pointerdown', close, true)
    const onResize = () => setOpen(false)
    window.addEventListener('resize', onResize)
    return () => {
      document.removeEventListener('pointerdown', close, true)
      window.removeEventListener('resize', onResize)
    }
  }, [open])

  const width = 300
  const toggle = () => {
    if (open) {
      setOpen(false)
      return
    }
    const rect = button.current?.getBoundingClientRect()
    if (!rect) return
    setSpot({
      left: Math.max(8, Math.min(rect.left, window.innerWidth - width - 8)),
      top: rect.bottom + 6,
    })
    setOpen(true)
  }

  const plan = presetPlan(preset)
  const actual = applied?.actual
  const codec = actual?.codec
  const codecLabel =
    codec === 'tiles'
      ? t('session.codecTiles')
      : codec
        ? codec === 'h264'
          ? 'H.264'
          : codec.toUpperCase()
        : '—'
  const actualLine = applied
    ? t('session.presetActual', {
        fps: actual?.fps ?? 0,
        codec: codecLabel,
        chroma: chromaLabel(actual?.chroma),
      })
    : t('session.presetPending')

  return (
    <div ref={holder} className="shrink-0">
      <button
        ref={button}
        type="button"
        onClick={toggle}
        className="btn btn-quiet flex h-7 shrink-0 items-center gap-0.5 !px-1.5"
        title={t('session.presetTitle')}
        aria-label={t('session.presetTitle')}
        aria-haspopup="dialog"
        aria-expanded={open}
      >
        <Gauge size={12} />
        <span className="text-[11px]">{t(plan.labelKey)}</span>
      </button>
      {open && spot
        ? createPortal(
            <div
              ref={surface}
              data-toolbar-menu="true"
              className="fixed z-50 flex flex-col gap-2 rounded-lg border border-line bg-surface p-2.5 shadow-xl"
              style={{ left: spot.left, top: spot.top, width }}
              role="dialog"
              aria-label={t('session.presetTitle')}
            >
              <div className="flex items-center justify-between">
                <span className="text-[11px] font-medium text-ink">
                  {t('session.presetTitle')}
                </span>
                <button
                  type="button"
                  className="btn btn-quiet h-6 w-6 !px-0"
                  aria-label={t('common.close')}
                  onClick={() => setOpen(false)}
                >
                  <X size={12} />
                </button>
              </div>
              <div className="grid grid-cols-3 gap-1">
                {SESSION_PRESETS.map((id) => {
                  // 自动 is deliberately separate from the five fixed tiers.
                  const item = presetPlan(id)
                  const selected = preset === id
                  return (
                    <button
                      key={id}
                      type="button"
                      onClick={() => {
                        onPreset(id)
                        setOpen(false)
                      }}
                      className={
                        'btn h-7 !px-1 text-[11px] ' +
                        (selected ? 'text-accent' : 'btn-quiet') +
                        (id === 'auto' ? ' col-start-1 mt-1' : '')
                      }
                      aria-pressed={selected}
                      title={t(item.descriptionKey)}
                    >
                      {t(item.labelKey)}
                    </button>
                  )
                })}
              </div>
              <div className="rounded-md border border-line bg-panel/60 px-2 py-1.5 text-[10px] leading-relaxed text-muted">
                {actualLine}
              </div>
              {preset === 'truecolor' && isExplicitStreamCodec(codecPreference)
                ? (
                    <p className="rounded-md border border-accent/30 bg-accent-soft px-2 py-1.5 text-[10px] leading-relaxed text-accent">
                      {t('session.truecolorWithCodec', {
                        codec: codecPreference === 'h264'
                          ? 'H.264'
                          : codecPreference?.toUpperCase(),
                      })}
                    </p>
                  )
                : null}
              <p className="text-[10px] leading-relaxed text-muted">
                {t('session.presetHint')}
              </p>
            </div>,
            document.body,
          )
        : null}
    </div>
  )
}
