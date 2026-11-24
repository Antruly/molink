/// Pointer input translation shared by the remote surface and its tests.
///
/// The host takes whole wheel notches, but a trackpad reports a few pixels at
/// a time (`deltaMode` 0) and a page can report lines or pages. Sending the
/// sign of every event made a 3 px trackpad step equal to a 120 px wheel
/// notch. The accumulator keeps the fraction and only sends whole notches.

export type WheelAccumulator = { x: number; y: number }

/// One wheel event's contribution in notches. Pixel deltas use the common
/// 100 px notch; a line is one notch and a page is three.
export function wheelDeltaToNotches(
  delta: number,
  deltaMode: number,
): number {
  if (!Number.isFinite(delta)) return 0
  if (deltaMode === 1) return delta
  if (deltaMode === 2) return delta * 3
  return delta / 100
}

export function createWheelAccumulator(): WheelAccumulator {
  return { x: 0, y: 0 }
}

export type WheelLike = {
  deltaX?: number
  deltaY?: number
  deltaMode?: number
  shiftKey?: boolean
}

/// Adds one WheelEvent to @p accumulator and returns the whole notches now
/// due. The fractional remainder stays in the accumulator. Shift+wheel is the
/// platform convention for horizontal scrolling.
export function wheelNotches(
  accumulator: WheelAccumulator,
  event: WheelLike,
): { dx: number; dy: number } {
  const mode = event.deltaMode ?? 0
  let ax = wheelDeltaToNotches(event.deltaX ?? 0, mode)
  let ay = wheelDeltaToNotches(event.deltaY ?? 0, mode)
  if (event.shiftKey) {
    ax += ay
    ay = 0
  }
  accumulator.x += ax
  accumulator.y += ay
  // `Math.trunc(-0.4)` is -0, and -0 on the wire is a needless surprise for
  // the host and for equality checks; normalise it to 0.
  const dx = Math.trunc(accumulator.x) || 0
  const dy = Math.trunc(accumulator.y) || 0
  accumulator.x -= dx
  accumulator.y -= dy
  return { dx, dy }
}

/// Browser `MouseEvent.button` to the host's numbering: 0/1/2 are left,
/// middle and right; 3 and 4 are the back and forward side buttons. Returns
/// null for buttons the host has no mapping for.
export function remoteButton(button: number): number | null {
  return Number.isInteger(button) && button >= 0 && button <= 4
    ? button + 1
    : null
}
