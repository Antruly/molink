import { useEffect, useRef, useState } from 'react'
import { createPortal } from 'react-dom'
import { useTranslation } from 'react-i18next'
import { ChevronDown, Gauge, X } from 'lucide-react'
import {
  ADVANCED_BITRATE_MAX,
  ADVANCED_CHROMAS,
  ADVANCED_CODECS,
  ADVANCED_FPS_MAX,
  ADVANCED_FPS_MIN,
  ADVANCED_QUALITY_MAX,
  SESSION_PRESETS,
  advancedFormatForChroma,
  advancedQualityHint,
  chromaLabel,
  isExplicitStreamCodec,
  presetPlan,
  type AdvancedQualityParams,
  type PresetApplied,
  type PresetChroma,
  type PresetRateMode,
  type SessionPresetCodec,
  type SessionPresetId,
} from '../lib/session-presets'

/// Stop a pointer/wheel/context-menu event from reaching the remote canvas.
/// The panel is portaled outside the picture today, but this keeps a nested
/// surface (or a future inline placement) from turning a slider drag into a
/// remote mouse drag.
function shield(event: { stopPropagation: () => void }) {
  event.stopPropagation()
}

const shieldProps = {
  onPointerDown: shield,
  onPointerMove: shield,
  onPointerUp: shield,
  onPointerCancel: shield,
  onWheel: shield,
  onContextMenu: shield,
}

/// A compact in-flow dropdown. Each option is a normal button at least 28 px
/// tall: an OS <select> popup cannot promise that in WebKitGTK, and the whole
/// point of this panel is that every control is reachable by pointer.
function AdvancedSelect<T extends string>({
  control,
  label,
  value,
  options,
  onChange,
}: {
  control: string
  label: string
  value: T
  options: readonly { value: T; label: string }[]
  onChange: (value: T) => void
}) {
  const [open, setOpen] = useState(false)
  const current = options.find((option) => option.value === value)
  return (
    <div className="space-y-1" data-advanced-row={control}>
      <div className="text-[11px] font-medium text-ink">{label}</div>
      <button
        type="button"
        className="btn btn-quiet flex h-8 w-full !justify-between !px-2 text-[11px]"
        aria-haspopup="listbox"
        aria-expanded={open}
        data-control={control}
        onClick={() => setOpen((value) => !value)}
      >
        <span>{current?.label ?? value}</span>
        <ChevronDown size={12} />
      </button>
      {open ? (
        <div
          role="listbox"
          aria-label={label}
          className="overflow-hidden rounded-md border border-line bg-surface"
        >
          {options.map((option) => (
            <button
              key={option.value}
              type="button"
              role="option"
              aria-selected={option.value === value}
              data-option={option.value}
              className={
                'flex min-h-7 w-full items-center px-2 text-left text-[11px] hover:bg-panel ' +
                (option.value === value ? 'text-accent' : 'text-ink')
              }
              onClick={() => {
                onChange(option.value)
                setOpen(false)
              }}
            >
              {option.label}
            </button>
          ))}
        </div>
      ) : null}
    </div>
  )
}

function RangeRow({
  control,
  label,
  value,
  min,
  max,
  step,
  left,
  right,
  onChange,
}: {
  control: string
  label: string
  value: number
  min: number
  max: number
  step: number
  left: string
  right: string
  onChange: (value: number) => void
}) {
  return (
    <div className="space-y-0.5" data-advanced-row={control}>
      <div className="flex items-center justify-between text-[11px]">
        <span className="font-medium text-ink">{label}</span>
        <span className="tabular-nums text-accent" data-advanced-value={control}>
          {value}
        </span>
      </div>
      <input
        type="range"
        className="quality-range"
        min={min}
        max={max}
        step={step}
        value={value}
        data-control={control}
        aria-label={label}
        onChange={(event) => onChange(Number(event.target.value))}
        onPointerDown={(event) => {
          // Keep the drag local. Do not call setPointerCapture() here: the
          // native range already captures the pointer itself, and an explicit
          // capture on WebKitGTK stops its internal thumb tracking (dragging
          // silently did nothing there).
          event.stopPropagation()
        }}
        onPointerMove={(event) => event.stopPropagation()}
        onPointerUp={(event) => event.stopPropagation()}
        onPointerCancel={(event) => event.stopPropagation()}
      />
      <div className="flex justify-between text-[9px] text-muted">
        <span>{left}</span>
        <span>{right}</span>
      </div>
    </div>
  )
}

/// The toolbar's 画质 group. In the default mode this is the viewer's picture
/// tier: fixed presets (plus a separate 自动), stored per host:port. With
/// advanced mode on the same popover becomes the per-device custom panel.
export function QualityPanel({
  preset,
  applied,
  codecPreference,
  advanced,
  advancedParams,
  codecOptions,
  onPreset,
  onAdvancedChange,
  onBackToPreset,
  onOpenChange,
}: {
  preset: SessionPresetId
  applied?: PresetApplied
  /// The host's global encoder preference. An explicit one is 4:2:0, so a
  /// truecolour tier is really running at reduced chroma.
  codecPreference?: string
  advanced: boolean
  advancedParams: AdvancedQualityParams
  /// Stream codecs the host advertised. The advanced panel adds 自动/平铺.
  codecOptions: string[]
  onPreset: (preset: SessionPresetId) => void
  onAdvancedChange: (patch: Partial<AdvancedQualityParams>) => void
  onBackToPreset: () => void
  /// Reports the popover state so keyboard forwarding can pause while it is
  /// open, exactly like the settings dialog.
  onOpenChange?: (open: boolean) => void
}) {
  const { t } = useTranslation()
  const [open, setOpen] = useState(false)
  const [spot, setSpot] = useState<{ left: number; top: number } | null>(null)
  const button = useRef<HTMLButtonElement>(null)
  const holder = useRef<HTMLDivElement>(null)
  const surface = useRef<HTMLDivElement>(null)
  const openChange = useRef(onOpenChange)
  // Updated in an effect rather than during render: writing to a ref while
  // rendering is what the compiler rejects, and the callback only matters once
  // a popover edge is reported.
  useEffect(() => {
    openChange.current = onOpenChange
  }, [onOpenChange])

  // Report both edges; the unmount cleanup covers closing the session while
  // the popover is still open.
  useEffect(() => {
    openChange.current?.(open)
  }, [open])
  useEffect(
    () => () => {
      openChange.current?.(false)
    },
    [],
  )

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
  const height = advanced ? 460 : 280
  const toggle = () => {
    if (open) {
      setOpen(false)
      return
    }
    const rect = button.current?.getBoundingClientRect()
    if (!rect) return
    setSpot({
      left: Math.max(8, Math.min(rect.left, window.innerWidth - width - 8)),
      top: Math.max(
        8,
        Math.min(rect.bottom + 6, window.innerHeight - height - 8),
      ),
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

  const hint = advancedQualityHint(advancedParams)
  // The quality slider is labelled 低 -> 高, while the wire value is QP
  // (lower is better). The two are mirrored, so the panel reads intuitively
  // and the host still receives the QP it documents.
  const qualityLevel = ADVANCED_QUALITY_MAX - advancedParams.quality

  const codecChoices: SessionPresetCodec[] = []
  const seen = new Set<string>(['auto'])
  codecChoices.push('auto')
  for (const value of codecOptions) {
    if (value === 'auto' || value === 'tiles') continue
    if (!(ADVANCED_CODECS as readonly string[]).includes(value)) continue
    if (seen.has(value)) continue
    seen.add(value)
    codecChoices.push(value as SessionPresetCodec)
  }
  // An older host may advertise nothing; still offer the documented set so
  // the panel is usable, and let the host reject an encoder it cannot run.
  if (codecChoices.length === 1) {
    for (const value of ADVANCED_CODECS) {
      if (value === 'auto' || value === 'tiles') continue
      codecChoices.push(value)
    }
  }
  if (!seen.has(advancedParams.codec)) {
    seen.add(advancedParams.codec)
    codecChoices.push(advancedParams.codec)
  }
  if (!seen.has('tiles')) {
    seen.add('tiles')
    codecChoices.push('tiles')
  }

  const chromaChoices: PresetChroma[] = [...ADVANCED_CHROMAS]
  const rateChoices: PresetRateMode[] = ['auto', 'cq', 'cbr']
  const codecText = (value: SessionPresetCodec) =>
    value === 'auto'
      ? t('session.codecAuto')
      : value === 'tiles'
        ? t('session.codecTiles')
        : value === 'h264'
          ? 'H.264'
          : value.toUpperCase()

  return (
    <div ref={holder} className="shrink-0">
      <button
        ref={button}
        type="button"
        onClick={toggle}
        className="btn btn-quiet flex h-7 shrink-0 items-center gap-0.5 !px-1.5"
        title={advanced ? t('session.advancedTitle') : t('session.presetTitle')}
        aria-label={advanced ? t('session.advancedTitle') : t('session.presetTitle')}
        aria-haspopup="dialog"
        aria-expanded={open}
      >
        <Gauge size={12} />
        <span className="text-[11px]">
          {advanced ? t('session.qualityCustom') : t(plan.labelKey)}
        </span>
      </button>
      {open && spot
        ? createPortal(
            <div
              ref={surface}
              data-toolbar-menu="true"
              data-local-input="true"
              data-advanced-panel={advanced ? 'true' : undefined}
              className="pointer-events-auto fixed z-40 flex max-h-[calc(100vh-1rem)] flex-col gap-2 overflow-y-auto rounded-lg border border-line bg-surface p-2.5 shadow-xl"
              style={{ left: spot.left, top: spot.top, width }}
              role="dialog"
              aria-label={
                advanced ? t('session.advancedTitle') : t('session.presetTitle')
              }
              {...shieldProps}
            >
              <div className="flex items-center justify-between">
                <span className="text-[11px] font-medium text-ink">
                  {advanced
                    ? t('session.advancedTitle')
                    : t('session.presetTitle')}
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

              {advanced ? (
                <>
                  <RangeRow
                    control="fps"
                    label={t('session.fps')}
                    value={advancedParams.fps}
                    min={ADVANCED_FPS_MIN}
                    max={ADVANCED_FPS_MAX}
                    step={1}
                    left={String(ADVANCED_FPS_MIN)}
                    right={String(ADVANCED_FPS_MAX)}
                    onChange={(fps) => onAdvancedChange({ fps })}
                  />
                  <RangeRow
                    control="quality"
                    label={t('session.advancedQuality')}
                    value={qualityLevel}
                    min={0}
                    max={ADVANCED_QUALITY_MAX}
                    step={1}
                    left={t('session.advancedLow')}
                    right={t('session.advancedHigh')}
                    onChange={(level) =>
                      onAdvancedChange({
                        quality: ADVANCED_QUALITY_MAX - level,
                      })
                    }
                  />

                  <div className="space-y-1" data-advanced-row="bitrate">
                    <div className="flex items-center justify-between text-[11px]">
                      <span className="font-medium text-ink">
                        {t('session.advancedBitrate')}
                      </span>
                      <span className="text-[9px] text-muted">
                        {t('session.advancedUnlimited')}
                      </span>
                    </div>
                    <input
                      type="number"
                      className="field h-8 w-full text-[11px]"
                      min={0}
                      max={ADVANCED_BITRATE_MAX}
                      step={100}
                      value={advancedParams.bitrateKbps}
                      data-control="bitrate"
                      aria-label={t('session.advancedBitrate')}
                      onChange={(event) => {
                        const value = Number(event.target.value)
                        onAdvancedChange({
                          bitrateKbps: Number.isFinite(value)
                            ? Math.max(
                                0,
                                Math.min(ADVANCED_BITRATE_MAX, Math.round(value)),
                              )
                            : 0,
                        })
                      }}
                    />
                  </div>

                  <div className="grid grid-cols-2 gap-2">
                    <AdvancedSelect<PresetChroma>
                      control="chroma"
                      label={t('session.advancedChroma')}
                      value={advancedParams.chroma}
                      options={chromaChoices.map((value) => ({
                        value,
                        label:
                          value === 'gray'
                            ? t('session.chromaGray')
                            : value === '565'
                              ? t('session.chroma565')
                              : chromaLabel(value),
                      }))}
                      onChange={(chroma) =>
                        onAdvancedChange({
                          chroma,
                          format: advancedFormatForChroma(
                            chroma,
                            advancedParams.format,
                          ),
                        })
                      }
                    />
                    <AdvancedSelect<SessionPresetCodec>
                      control="codec"
                      label={t('session.advancedCodec')}
                      value={advancedParams.codec}
                      options={codecChoices.map((value) => ({
                        value,
                        label: codecText(value),
                      }))}
                      onChange={(next) => onAdvancedChange({ codec: next })}
                    />
                  </div>
                  <AdvancedSelect<PresetRateMode>
                    control="rateMode"
                    label={t('session.rateMode')}
                    value={advancedParams.rateMode}
                    options={rateChoices.map((value) => ({
                      value,
                      label:
                        value === 'auto'
                          ? t('session.rateModeAuto')
                          : value === 'cq'
                            ? t('session.rateModeCq')
                            : t('session.rateModeCbr'),
                    }))}
                    onChange={(rateMode) => onAdvancedChange({ rateMode })}
                  />

                  <div className="rounded-md border border-line bg-panel/60 px-2 py-1.5 text-[10px] leading-relaxed text-muted">
                    {actualLine}
                  </div>
                  {hint === 'forced420' ? (
                    <p className="rounded-md border border-accent/30 bg-accent-soft px-2 py-1.5 text-[10px] leading-relaxed text-accent">
                      {t('session.advancedChromaForced')}
                    </p>
                  ) : hint === 'tiles' ? (
                    <p className="rounded-md border border-accent/30 bg-accent-soft px-2 py-1.5 text-[10px] leading-relaxed text-accent">
                      {t('session.advancedTilesHint')}
                    </p>
                  ) : null}
                  <button
                    type="button"
                    className="btn h-8 w-full text-[11px]"
                    data-control="back-to-preset"
                    onClick={onBackToPreset}
                  >
                    {t('session.advancedBackToPreset')}
                  </button>
                  <p className="text-[10px] leading-relaxed text-muted">
                    {t('session.advancedHint')}
                  </p>
                </>
              ) : (
                <>
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
                  {preset === 'truecolor' &&
                  isExplicitStreamCodec(codecPreference)
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
                </>
              )}
            </div>,
            document.body,
          )
        : null}
    </div>
  )
}
