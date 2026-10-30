import { useEffect, useRef, useState } from 'react'
import { createPortal } from 'react-dom'
import { useTranslation } from 'react-i18next'
import { Gauge, RotateCcw, X } from 'lucide-react'

/// Toolbar slider values. 0 (or "preset") means "follow the quality preset";
/// the host treats every slider as a preference it may adjust around, not a
/// fixed value.
export interface EncodingKnobs {
  qp: number
  bitrateKbps: number
  rateMode: 'auto' | 'cq' | 'cbr'
  format: string
}

export const defaultKnobs: EncodingKnobs = {
  qp: 0,
  bitrateKbps: 0,
  rateMode: 'auto',
  format: 'preset',
}

const PRESETS = [
  'turbo',
  'smooth',
  'balanced',
  'quality',
  'ultra',
  'lossless',
  'auto',
] as const

const PRESET_LABEL: Record<string, string> = {
  turbo: 'session.qualityTurbo',
  smooth: 'session.qualitySmooth',
  balanced: 'session.qualityBalanced',
  quality: 'session.qualityHigh',
  ultra: 'session.qualityUltra',
  lossless: 'session.qualityLossless',
  auto: 'session.qualityAuto',
}

const FORMATS = ['preset', 'gray8', 'rgb565', 'bgra'] as const

const FORMAT_LABEL: Record<string, string> = {
  preset: 'session.formatPreset',
  gray8: 'session.formatMono',
  rgb565: 'session.format16',
  bgra: 'session.formatTrue',
}

/// The toolbar's 画质 group: preset chips on top, then the rate-mode switch
/// and the per-knob sliders. Moving any slider makes the tier "custom" -- the
/// preset stays the baseline the host may adapt around.
export function QualityPanel({
  mode,
  custom,
  knobs,
  fps,
  onMode,
  onKnobs,
  onFps,
}: {
  mode: string
  custom: boolean
  knobs: EncodingKnobs
  fps: number
  onMode: (mode: string) => void
  onKnobs: (knobs: EncodingKnobs) => void
  onFps: (fps: number) => void
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

  const width = 316
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

  const label = custom ? t('session.qualityCustom') : t(PRESET_LABEL[mode] ?? 'session.quality')
  const sliderRow = (
    name: string,
    value: number,
    min: number,
    max: number,
    display: string,
    onChange: (value: number) => void,
    inverted = false,
  ) => (
    <label className="flex items-center gap-2 text-[11px] text-muted">
      <span className="w-14 shrink-0">{name}</span>
      <input
        type="range"
        min={min}
        max={max}
        value={value}
        onChange={(event) => onChange(Number(event.target.value))}
        className={'h-1 min-w-0 flex-1 accent-accent ' + (inverted ? 'direction-rtl' : '')}
        dir={inverted ? 'rtl' : 'ltr'}
      />
      <span className="w-20 shrink-0 text-right text-ink">{display}</span>
    </label>
  )

  return (
    <div ref={holder} className="shrink-0">
      <button
        ref={button}
        type="button"
        onClick={toggle}
        className="btn btn-quiet flex h-7 shrink-0 items-center gap-0.5 !px-1.5"
        title={t('session.quality')}
        aria-label={t('session.quality')}
        aria-haspopup="dialog"
        aria-expanded={open}
      >
        <Gauge size={12} />
        <span className="text-[11px]">{label}</span>
      </button>
      {open && spot
        ? createPortal(
            <div
              ref={surface}
              className="fixed z-50 flex flex-col gap-2 rounded-lg border border-line bg-surface p-2.5 shadow-xl"
              style={{ left: spot.left, top: spot.top, width }}
              role="dialog"
              aria-label={t('session.quality')}
            >
              <div className="flex items-center justify-between">
                <span className="text-[11px] font-medium text-ink">
                  {t('session.quality')}
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
              <div className="grid grid-cols-4 gap-1">
                {PRESETS.map((preset) => (
                  <button
                    key={preset}
                    type="button"
                    onClick={() => {
                      onMode(preset)
                      onKnobs(defaultKnobs)
                    }}
                    className={
                      'btn h-6 !px-1 text-[10px] ' +
                      (!custom && mode === preset ? 'text-accent' : 'btn-quiet')
                    }
                    aria-pressed={!custom && mode === preset}
                  >
                    {t(PRESET_LABEL[preset])}
                  </button>
                ))}
              </div>
              <div className="flex items-center gap-1">
                <span className="w-14 shrink-0 text-[11px] text-muted">
                  {t('session.rateMode')}
                </span>
                {(['auto', 'cq', 'cbr'] as const).map((value) => (
                  <button
                    key={value}
                    type="button"
                    onClick={() => onKnobs({ ...knobs, rateMode: value })}
                    className={
                      'btn h-6 flex-1 !px-1 text-[10px] ' +
                      (knobs.rateMode === value ? 'text-accent' : 'btn-quiet')
                    }
                    aria-pressed={knobs.rateMode === value}
                  >
                    {t(
                      value === 'auto'
                        ? 'session.rateModeAuto'
                        : value === 'cq'
                          ? 'session.rateModeCq'
                          : 'session.rateModeCbr',
                    )}
                  </button>
                ))}
              </div>
              {sliderRow(
                t('session.knobQuality'),
                knobs.qp,
                0,
                63,
                knobs.qp === 0 ? t('session.knobFollowPreset') : String(knobs.qp),
                (qp) => onKnobs({ ...knobs, qp }),
                true,
              )}
              {sliderRow(
                t('session.knobBitrate'),
                knobs.bitrateKbps,
                0,
                20000,
                knobs.bitrateKbps === 0
                  ? t('session.knobFollowPreset')
                  : `${knobs.bitrateKbps} kbps`,
                (bitrateKbps) => onKnobs({ ...knobs, bitrateKbps }),
              )}
              {sliderRow(
                t('session.knobFps'),
                fps,
                1,
                60,
                `${fps} fps`,
                (value) => onFps(value),
              )}
              <label className="flex items-center gap-2 text-[11px] text-muted">
                <span className="w-14 shrink-0">{t('session.imageFormat')}</span>
                <select
                  value={knobs.format}
                  onChange={(event) =>
                    onKnobs({ ...knobs, format: event.target.value })
                  }
                  className="h-6 min-w-0 flex-1 bg-transparent text-[11px] text-ink outline-none [&>option]:bg-surface [&>option]:text-ink"
                  aria-label={t('session.imageFormat')}
                >
                  {FORMATS.map((format) => (
                    <option key={format} value={format}>
                      {t(FORMAT_LABEL[format])}
                    </option>
                  ))}
                </select>
              </label>
              <button
                type="button"
                onClick={() => onKnobs(defaultKnobs)}
                className="btn btn-quiet flex h-6 items-center justify-center gap-1 text-[10px]"
              >
                <RotateCcw size={11} />
                {t('session.qualityReset')}
              </button>
            </div>,
            document.body,
          )
        : null}
    </div>
  )
}
