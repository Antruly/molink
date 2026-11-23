/**
 * The viewer's picture presets.
 *
 * These are *viewer* settings: the machine showing the remote desktop owns
 * them, per host:port, and the host only ever reports what it actually
 * applied. They are fixed presets, not live sliders: picking one resolves to
 * one encoding mode plus a capture rate, and the host must not switch it back.
 *
 * The wire values are the host's existing quality modes, so no host-side
 * preset table has to grow. The colour tiers (turbo/smooth/balanced/hd/auto)
 * improve the picture through QP, bitrate and frame rate while staying on the
 * 4:2:0 video stream. Only `truecolor` asks for 4:4:4, and it does so with an
 * explicit BGRA format: the host keeps the tile path (no chroma subsampling)
 * and does not start a 4:2:0 video stream for it.
 */
export type SessionPresetId =
  | 'turbo'
  | 'smooth'
  | 'balanced'
  | 'hd'
  | 'truecolor'
  | 'auto'

/// Quality modes the host understands. `auto` is viewer-side only: when the
/// viewer picks auto it sends the literal `auto` preset and the host may walk
/// its own ladder.
export type PresetQualityMode =
  | 'turbo'
  | 'smooth'
  | 'balanced'
  | 'quality'
  | 'lossless'

export type PresetChroma = '420' | '444' | 'gray' | '565' | 'auto'
export type PresetRateMode = 'auto' | 'cq' | 'cbr'
export type PresetFormat = 'preset' | 'gray8' | 'rgb565' | 'rgb24' | 'bgra'
export type SessionPresetCodec =
  | 'auto'
  | 'av1'
  | 'vp9'
  | 'vp8'
  | 'h264'
  | 'tiles'

export interface SessionPresetPlan {
  id: SessionPresetId
  /// Label/description keys in `session.*`.
  labelKey: string
  descriptionKey: string
  /// Quality mode sent to the host (auto resolves to the host's ladder).
  mode: PresetQualityMode
  /// Resting capture rate; the load controller may lower it, never the host.
  fps: number
  minFps: number
  maxFps: number
  /// QP sent to the host (0 = follow the quality profile).
  qp: number
  /// Stream bitrate ceiling in kbit/s (0 = follow the profile).
  bitrateKbps: number
  rateMode: PresetRateMode
  format: PresetFormat
  /// Requested chroma. Only truecolour asks for 444 (with `format: 'bgra'`):
  /// that keeps the pixel-exact tile path and suppresses the 4:2:0 stream.
  /// `auto` lets the host's selection stand.
  chroma: PresetChroma
}

/// Order shown in the toolbar and settings. "自动" sits after the five fixed
/// tiers, deliberately apart from them.
export const SESSION_PRESETS: readonly SessionPresetId[] = [
  'turbo',
  'smooth',
  'balanced',
  'hd',
  'truecolor',
  'auto',
] as const

export const FIXED_SESSION_PRESETS: readonly SessionPresetId[] = [
  'turbo',
  'smooth',
  'balanced',
  'hd',
  'truecolor',
] as const

export const DEFAULT_SESSION_PRESET: SessionPresetId = 'smooth'

export const PRESET_PLANS: Record<SessionPresetId, SessionPresetPlan> = {
  turbo: {
    id: 'turbo',
    labelKey: 'session.presetTurbo',
    descriptionKey: 'session.presetTurboDesc',
    mode: 'turbo',
    fps: 10,
    minFps: 5,
    maxFps: 15,
    qp: 0,
    bitrateKbps: 2500,
    rateMode: 'auto',
    format: 'preset',
    chroma: 'gray',
  },
  smooth: {
    id: 'smooth',
    labelKey: 'session.presetSmooth',
    descriptionKey: 'session.presetSmoothDesc',
    mode: 'smooth',
    fps: 24,
    minFps: 8,
    maxFps: 30,
    qp: 0,
    bitrateKbps: 6000,
    rateMode: 'auto',
    format: 'preset',
    chroma: '420',
  },
  balanced: {
    id: 'balanced',
    labelKey: 'session.presetBalanced',
    descriptionKey: 'session.presetBalancedDesc',
    mode: 'balanced',
    fps: 30,
    minFps: 10,
    maxFps: 30,
    qp: 0,
    bitrateKbps: 0,
    rateMode: 'auto',
    format: 'preset',
    chroma: '420',
  },
  hd: {
    id: 'hd',
    labelKey: 'session.presetHd',
    descriptionKey: 'session.presetHdDesc',
    mode: 'quality',
    fps: 45,
    minFps: 15,
    maxFps: 60,
    qp: 0,
    bitrateKbps: 0,
    rateMode: 'auto',
    format: 'preset',
    chroma: '420',
  },
  truecolor: {
    id: 'truecolor',
    labelKey: 'session.presetTruecolor',
    descriptionKey: 'session.presetTruecolorDesc',
    mode: 'lossless',
    fps: 18,
    minFps: 10,
    maxFps: 20,
    qp: 0,
    bitrateKbps: 0,
    rateMode: 'cq',
    format: 'bgra',
    chroma: '444',
  },
  auto: {
    id: 'auto',
    labelKey: 'session.presetAuto',
    descriptionKey: 'session.presetAutoDesc',
    mode: 'balanced',
    fps: 30,
    minFps: 5,
    maxFps: 30,
    qp: 0,
    bitrateKbps: 0,
    rateMode: 'auto',
    format: 'preset',
    chroma: 'auto',
  },
}

export function presetPlan(id: SessionPresetId): SessionPresetPlan {
  return PRESET_PLANS[id] ?? PRESET_PLANS[DEFAULT_SESSION_PRESET]
}

/// Unknown or missing ids (an older/newer UI, hand-edited storage) fall back
/// to the default instead of breaking a session.
export function normalizePresetId(value: unknown): SessionPresetId {
  if (typeof value !== 'string') return DEFAULT_SESSION_PRESET
  return (SESSION_PRESETS as readonly string[]).includes(value)
    ? (value as SessionPresetId)
    : DEFAULT_SESSION_PRESET
}

export function isFixedPreset(id: SessionPresetId): boolean {
  return id !== 'auto'
}

/// The advanced panel's own parameters. Unlike the six fixed tiers these are
/// not derived from a plan on every send: the viewer owns them per host:port
/// and the host only reports what it applied.
export interface AdvancedQualityParams {
  /// 5..60 capture rate.
  fps: number
  /// QP 0..63; the panel labels 63 as the low-quality end (0 follows the
  /// quality profile).
  quality: number
  /// Stream bitrate ceiling in kbit/s, 0 = no ceiling.
  bitrateKbps: number
  rateMode: PresetRateMode
  format: PresetFormat
  /// 420 / 444 / gray / 565. `auto` is a preset-only value.
  chroma: PresetChroma
  codec: SessionPresetCodec
}

export const ADVANCED_STORAGE_PREFIX = 'molink.advanced.'
export const ADVANCED_QUALITY_KEY = 'molink.advancedQuality'
export const ADVANCED_FPS_MIN = 5
export const ADVANCED_FPS_MAX = 60
export const ADVANCED_QUALITY_MAX = 63
export const ADVANCED_BITRATE_MAX = 50000
/// The toolbar debounces every advanced change for this long before sending
/// `session-preset`; dragging a slider must not rebuild the encoder per pixel.
export const ADVANCED_DEBOUNCE_MS = 300

export const ADVANCED_CODECS: readonly SessionPresetCodec[] = [
  'auto',
  'av1',
  'vp9',
  'vp8',
  'h264',
  'tiles',
] as const

export const ADVANCED_CHROMAS: readonly PresetChroma[] = [
  '420',
  '444',
  'gray',
  '565',
] as const

const RATE_MODES: readonly PresetRateMode[] = ['auto', 'cq', 'cbr']
const FORMATS: readonly PresetFormat[] = [
  'preset',
  'gray8',
  'rgb565',
  'rgb24',
  'bgra',
]

/// The value the advanced panel starts from when this device has no saved
/// custom choice: the currently selected preset tier, exactly.
export function defaultAdvancedParams(
  preset: SessionPresetId = DEFAULT_SESSION_PRESET,
): AdvancedQualityParams {
  const plan = presetPlan(preset)
  return {
    fps: plan.fps,
    quality: plan.qp,
    bitrateKbps: plan.bitrateKbps,
    rateMode: plan.rateMode,
    format: plan.format,
    chroma: plan.chroma,
    codec: 'auto',
  }
}

function clampInt(value: unknown, min: number, max: number, fallback: number) {
  if (typeof value !== 'number' || !Number.isFinite(value)) return fallback
  return Math.max(min, Math.min(max, Math.round(value)))
}

function oneOf<T extends string>(
  value: unknown,
  allowed: readonly T[],
  fallback: T,
): T {
  return typeof value === 'string' && (allowed as readonly string[]).includes(value)
    ? (value as T)
    : fallback
}

/// A stored value is untrusted (hand-edited localStorage, an older ui.json).
/// Numbers are clamped and enums fall back one field at a time, so one bad
/// value never throws the rest away.
export function normalizeAdvancedParams(
  value: unknown,
  fallback: AdvancedQualityParams = defaultAdvancedParams(),
): AdvancedQualityParams {
  const parsed =
    value && typeof value === 'object'
      ? (value as Partial<AdvancedQualityParams>)
      : {}
  return {
    fps: clampInt(
      parsed.fps,
      ADVANCED_FPS_MIN,
      ADVANCED_FPS_MAX,
      fallback.fps,
    ),
    quality: clampInt(
      parsed.quality,
      0,
      ADVANCED_QUALITY_MAX,
      fallback.quality,
    ),
    bitrateKbps: clampInt(
      parsed.bitrateKbps,
      0,
      ADVANCED_BITRATE_MAX,
      fallback.bitrateKbps,
    ),
    rateMode: oneOf(parsed.rateMode, RATE_MODES, fallback.rateMode),
    format: oneOf(parsed.format, FORMATS, fallback.format),
    chroma: oneOf(parsed.chroma, ADVANCED_CHROMAS, fallback.chroma),
    codec: oneOf(parsed.codec, ADVANCED_CODECS, fallback.codec),
  }
}

export function advancedStorageKey(host: string, port: number): string {
  return `${ADVANCED_STORAGE_PREFIX}${host.trim()}:${port}`
}

export function parseStoredAdvanced(
  raw: string | null,
  fallback: AdvancedQualityParams = defaultAdvancedParams(),
): AdvancedQualityParams | null {
  if (!raw) return null
  try {
    return normalizeAdvancedParams(JSON.parse(raw), fallback)
  } catch {
    return null
  }
}

export function readStoredAdvanced(
  host: string,
  port: number,
): AdvancedQualityParams | null {
  try {
    if (typeof window === 'undefined' || !window.localStorage) return null
    return parseStoredAdvanced(
      window.localStorage.getItem(advancedStorageKey(host, port)),
    )
  } catch {
    return null
  }
}

export function writeStoredAdvanced(
  host: string,
  port: number,
  value: AdvancedQualityParams,
): void {
  try {
    if (typeof window === 'undefined' || !window.localStorage) return
    window.localStorage.setItem(
      advancedStorageKey(host, port),
      JSON.stringify(normalizeAdvancedParams(value)),
    )
  } catch {
    // Ignore blocked storage; the in-memory store still works.
  }
}

/// A tile format that matches the requested chroma. The panel has no format
/// picker: 4:4:4 means the pixel-exact BGRA path, grayscale means Gray8 and
/// 16-bit means RGB565; 4:2:0 leaves the format to the profile.
export function advancedFormatForChroma(
  chroma: PresetChroma,
  current: PresetFormat = 'preset',
): PresetFormat {
  switch (chroma) {
    case '444':
      return current === 'rgb24' ? 'rgb24' : 'bgra'
    case 'gray':
      return 'gray8'
    case '565':
      return 'rgb565'
    case '420':
      return current === 'bgra' ||
        current === 'rgb24' ||
        current === 'gray8' ||
        current === 'rgb565'
        ? 'preset'
        : current
    default:
      return current
  }
}

/// The exact `session-preset` payload for the advanced panel. An explicit
/// stream encoder is 4:2:0, so a 4:4:4 request is sent as 4:2:0 (the backend
/// enforces the same rule); an explicit `tiles` codec keeps the chroma.
export function advancedPresetMessage(
  params: AdvancedQualityParams,
): SessionPresetMessage {
  const codec = params.codec
  const forced420 = params.chroma === '444' && isExplicitStreamCodec(codec)
  const chroma: PresetChroma = forced420 ? '420' : params.chroma
  return {
    type: 'session-preset',
    preset: 'custom',
    fps: clampInt(params.fps, ADVANCED_FPS_MIN, ADVANCED_FPS_MAX, 30),
    quality: clampInt(params.quality, 0, ADVANCED_QUALITY_MAX, 0),
    bitrate_kbps: clampInt(
      params.bitrateKbps,
      0,
      ADVANCED_BITRATE_MAX,
      0,
    ),
    rate_mode: params.rateMode,
    format: advancedFormatForChroma(chroma, params.format),
    chroma,
    codec,
  }
}

/// What the panel must warn about after a change: `forced420` when the chosen
/// encoder cannot carry 4:4:4, `tiles` when the combination leaves the video
/// stream for the tile path.
export function advancedQualityHint(
  params: AdvancedQualityParams,
): 'tiles' | 'forced420' | null {
  if (params.chroma === '444' && isExplicitStreamCodec(params.codec)) {
    return 'forced420'
  }
  if (params.codec === 'tiles' || params.chroma === '444') return 'tiles'
  return null
}

export interface AdvancedPresetPump {
  /// Coalesces rapid changes: only the last message is sent, after `delay`.
  update(message: SessionPresetMessage): void
  cancel(): void
}

export interface AdvancedPresetTimers {
  set: (callback: () => void, ms: number) => unknown
  clear: (handle: unknown) => void
}

/// The debounce used by the toolbar. Kept outside React so the acceptance
/// test can drive it with a fake clock; the effect only calls update()/cancel().
export function createAdvancedPresetPump(
  send: (message: SessionPresetMessage) => void,
  delay: number = ADVANCED_DEBOUNCE_MS,
  timers: AdvancedPresetTimers = {
    set: (callback, ms) => globalThis.setTimeout(callback, ms),
    clear: (handle) =>
      globalThis.clearTimeout(handle as ReturnType<typeof setTimeout>),
  },
): AdvancedPresetPump {
  let handle: unknown = null
  let pending: SessionPresetMessage | null = null
  const cancel = () => {
    if (handle !== null) timers.clear(handle)
    handle = null
    pending = null
  }
  return {
    update(message) {
      cancel()
      pending = message
      handle = timers.set(() => {
        handle = null
        const next = pending
        pending = null
        if (next) send(next)
      }, delay)
    },
    cancel,
  }
}

/// A stored per-host selection. `fps` keeps the user's last rate for auto;
/// the fixed presets always resolve their rate from the plan.
export interface StoredSessionPreset {
  preset: SessionPresetId
  fps?: number
  updatedAt?: number
}

export const PRESET_STORAGE_PREFIX = 'molink.preset.'

export function presetStorageKey(host: string, port: number): string {
  return `${PRESET_STORAGE_PREFIX}${host.trim()}:${port}`
}

export function parseStoredPreset(raw: string | null): StoredSessionPreset | null {
  if (!raw) return null
  try {
    const parsed = JSON.parse(raw) as Partial<StoredSessionPreset>
    const preset = normalizePresetId(parsed.preset)
    const fps =
      typeof parsed.fps === 'number' && Number.isFinite(parsed.fps)
        ? Math.max(1, Math.min(60, Math.round(parsed.fps)))
        : undefined
    return {
      preset,
      ...(fps !== undefined ? { fps } : {}),
      ...(typeof parsed.updatedAt === 'number'
        ? { updatedAt: parsed.updatedAt }
        : {}),
    }
  } catch {
    // A hand-edited or truncated value is not worth a broken session.
    return null
  }
}

/// localStorage is origin-scoped, so the native shell also mirrors this map
/// into ui.json (see the store's `presets`). Both helpers are no-ops when
/// storage is unavailable (private windows, tests).
export function readStoredPreset(
  host: string,
  port: number,
): StoredSessionPreset | null {
  try {
    if (typeof window === 'undefined' || !window.localStorage) return null
    return parseStoredPreset(
      window.localStorage.getItem(presetStorageKey(host, port)),
    )
  } catch {
    return null
  }
}

export function writeStoredPreset(
  host: string,
  port: number,
  value: StoredSessionPreset,
): void {
  try {
    if (typeof window === 'undefined' || !window.localStorage) return
    window.localStorage.setItem(
      presetStorageKey(host, port),
      JSON.stringify(value),
    )
  } catch {
    // Ignore blocked storage; the in-memory store still works.
  }
}

/// The message the viewer sends after the welcome and on every change.
export interface SessionPresetMessage {
  type: 'session-preset'
  /// `custom` means every field below is the viewer's own value; the six
  /// named tiers keep resolving their fields from the plan.
  preset: SessionPresetId | 'custom'
  fps: number
  quality: number
  bitrate_kbps: number
  rate_mode: PresetRateMode
  format: PresetFormat
  chroma: PresetChroma
  codec: SessionPresetCodec
}

/// Resolves the selected preset into the exact wire payload. `fps` is the
/// viewer's current target (the load controller may have lowered it); for the
/// fixed tiers it starts at the plan's rate.
///
/// Every field is rebuilt from the plan on each call. Nothing from the
/// previously selected tier (its chroma, BGRA format, bitrate or rate mode)
/// is carried over: switching truecolor -> auto must send the auto plan's
/// values, not truecolor's.
export function presetMessage(
  preset: SessionPresetId,
  fps: number,
  codec: 'auto' = 'auto',
): SessionPresetMessage {
  const plan = presetPlan(preset)
  const rate = Math.max(1, Math.min(60, Math.round(fps || plan.fps)))
  return {
    type: 'session-preset',
    preset,
    fps: rate,
    quality: plan.qp,
    bitrate_kbps: plan.bitrateKbps,
    rate_mode: plan.rateMode,
    format: plan.format,
    chroma: plan.chroma,
    codec,
  }
}

export interface PresetActual {
  fps: number
  quality?: string
  format?: string
  chroma?: string
  codec?: string
}

/// What the host actually turned on, as reported by its `session-preset`
/// reply. The UI shows these values; they never move the selector.
export interface PresetApplied {
  applied: true
  preset?: string
  actual: PresetActual
}

/// The host's reply is an observation, not a command. Returning the viewer's
/// selection here is the whole "被改回去" fix: a host echo (or another
/// viewer's choice) can update the small actual-values line but cannot change
/// the selected tier.
export function viewerPresetAfterReply(
  selected: SessionPresetId,
  reply?: { preset?: string } | null,
): SessionPresetId {
  if (reply?.preset && reply.preset !== selected) {
    // Observed only. The host applied something else (or an older host echoed
    // its global default); the viewer keeps its own choice and will send it
    // again on the next change.
  }
  return selected
}

/// True for the four real stream encoders. "auto" and "tiles" are modes, not
/// encoders: auto lets the host adapt, tiles is the explicit never-stream
/// choice.
export function isExplicitStreamCodec(codec?: string): boolean {
  return codec === 'av1' || codec === 'h264' || codec === 'vp9' ||
    codec === 'vp8'
}

/// The chroma a preset actually gets when the host has pinned an encoder.
/// Every stream encoder here is 4:2:0, so truecolour's 4:4:4 gives way to
/// 4:2:0 rather than silently leaving the stream off.
export function effectivePresetChroma(
  chroma: PresetChroma,
  codec?: string,
): PresetChroma {
  return isExplicitStreamCodec(codec) && chroma === '444' ? '420' : chroma
}

/// True when selecting `codec` would leave the selected picture tier on the
/// tile path and must therefore warn the viewer about the exit.
export function codecExitsTruecolor(
  preset: SessionPresetId,
  codec?: string,
): boolean {
  return preset === 'truecolor' && isExplicitStreamCodec(codec)
}

export function chromaLabel(chroma?: string): string {
  switch (chroma) {
    case '444':
      return '4:4:4'
    case '420':
      return '4:2:0'
    case 'gray':
    case 'gray8':
      return '8-bit'
    case '565':
    case 'rgb565':
      return '16-bit'
    default:
      return '—'
  }
}

export function formatLabel(format?: string): string {
  switch (format) {
    case 'bgra':
      return 'BGRA'
    case 'rgb24':
      return 'RGB24'
    case 'rgb565':
      return 'RGB565'
    case 'gray8':
      return 'Gray8'
    default:
      return '—'
  }
}
