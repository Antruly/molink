/**
 * Average frame rate, independent from the quality preset.
 *
 * A client picks a fixed rate (5/10/15/25/45/60) or "auto". Auto derives a
 * window from the active quality tier: the session starts at its resting value
 * and the load controller lowers the rate when the host cannot encode fast
 * enough or the link cannot carry it, then climbs back towards the tier's
 * ceiling. A fixed rate is only ever lowered under load, never raised past the
 * chosen value.
 */
export type QualityTier =
  | 'turbo'
  | 'smooth'
  | 'balanced'
  | 'quality'
  | 'ultra'
  | 'lossless'
  | 'game'
  | 'raw'

export type FpsSetting = 'auto' | 5 | 10 | 15 | 25 | 45 | 60

export const FPS_CHOICES: FpsSetting[] = ['auto', 5, 10, 15, 25, 45, 60]

export const MIN_FPS = 5
export const MAX_FPS = 60

export interface FpsPlan {
  /** Lowest rate automatic load reduction may reach. */
  min: number
  /// The tier's resting floor: the rate the healthy controller works around
  /// and the point at which auto quality starts downgrading the tier.
  floor: number
  /** Highest rate this choice may reach. */
  max: number
  /** Where a fresh session (or a new choice) starts. */
  start: number
}

/// Auto windows per quality tier. "Quality only controls quality": the tier
/// picks the window, the link decides where inside it the session settles.
/// The absolute floor stays 5 for every tier: a host that cannot encode, or a
/// link that cannot carry the picture, always wins over the tier's resting
/// rate.
const AUTO_PLANS: Record<QualityTier, FpsPlan> = {
  // Weak-link tier: a wide window so the controller can follow the link.
  turbo: { min: 5, floor: 5, max: 25, start: 15 },
  smooth: { min: 5, floor: 10, max: 25, start: 15 },
  balanced: { min: 5, floor: 10, max: 25, start: 15 },
  // Sharper tiers stay at the default weight; they may still drop under load.
  quality: { min: 5, floor: 15, max: 15, start: 15 },
  ultra: { min: 5, floor: 15, max: 15, start: 15 },
  lossless: { min: 5, floor: 15, max: 15, start: 15 },
  raw: { min: 5, floor: 15, max: 15, start: 15 },
  game: { min: 5, floor: 30, max: 60, start: 30 },
}

export function clampFps(value: number): number {
  if (!Number.isFinite(value)) return 15
  return Math.max(MIN_FPS, Math.min(MAX_FPS, Math.round(value)))
}

export function fpsPlan(setting: FpsSetting, tier: QualityTier): FpsPlan {
  if (setting === 'auto') return AUTO_PLANS[tier] ?? AUTO_PLANS.balanced
  const value = clampFps(setting)
  return { min: MIN_FPS, floor: value, max: value, start: value }
}

/// localStorage stores the choice as text; unknown values fall back to auto so
/// an older/newer UI cannot break the session.
export function parseFpsSetting(raw: string | null): FpsSetting {
  if (raw === null) return 'auto'
  if (raw === 'auto') return 'auto'
  const value = Number(raw)
  const match = FPS_CHOICES.find((choice) => choice === value)
  return match ?? 'auto'
}
