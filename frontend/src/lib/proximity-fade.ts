/**
 * Proximity geometry for the session overlays.
 *
 * The picture's corner badges, the bottom stats bar and the connecting
 * watermark each fade out when the pointer comes close. "Close" means the
 * pointer is inside the element's own rectangle expanded by a fixed radius
 * (the corners are square, not rounded: a small radius around a small badge
 * makes the two indistinguishable, and a square is cheaper to test).
 */
export interface FadeRect {
  left: number
  top: number
  right: number
  bottom: number
}

/** How far outside an overlay the pointer may be and still fade it. */
export const DEFAULT_FADE_RADIUS = 28

/** Debounce before a faded overlay comes back, so moving between two
 * overlays or brushing an edge does not flicker. */
export const DEFAULT_RESTORE_DELAY_MS = 80

/** The class used while faded; the transition itself lives on the element. */
export const FADE_OPACITY_CLASS = 'opacity-[0.08]'

/**
 * True when (x, y) is inside `rect` expanded by `radius` on every side.
 * Boundaries are inclusive. Degenerate rectangles (a hidden element reports
 * 0×0) never match, and non-finite input never matches.
 */
export function isNearRect(
  x: number,
  y: number,
  rect: FadeRect,
  radius: number = DEFAULT_FADE_RADIUS,
): boolean {
  if (!Number.isFinite(x) || !Number.isFinite(y)) return false
  if (!Number.isFinite(rect.left) || !Number.isFinite(rect.top)) return false
  if (!Number.isFinite(rect.right) || !Number.isFinite(rect.bottom)) return false
  if (!(rect.right > rect.left) || !(rect.bottom > rect.top)) return false
  const reach = Math.max(0, Number.isFinite(radius) ? radius : 0)
  return (
    x >= rect.left - reach &&
    x <= rect.right + reach &&
    y >= rect.top - reach &&
    y <= rect.bottom + reach
  )
}

/**
 * Independent hit test for several overlays. Each target keeps its own
 * verdict: being near the corner badges says nothing about the stats bar.
 */
export function fadedTargets<K extends string>(
  x: number,
  y: number,
  rects: Record<K, FadeRect | null | undefined>,
  radius: number = DEFAULT_FADE_RADIUS,
): Record<K, boolean> {
  const result = {} as Record<K, boolean>
  for (const key of Object.keys(rects) as K[]) {
    const rect = rects[key]
    result[key] = rect ? isNearRect(x, y, rect, radius) : false
  }
  return result
}
