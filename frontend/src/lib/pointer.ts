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

/// A position on the picture in [0, 1] units, rounded the way it goes on the
/// wire.
export type PicturePoint = { x: number; y: number }

/// Clamps a normalized position to the picture's edges.
export function clampPoint(nx: number, ny: number): PicturePoint {
  const clamp = (value: number) => Math.min(1, Math.max(0, value))
  return {
    x: Number(clamp(nx).toFixed(5)),
    y: Number(clamp(ny).toFixed(5)),
  }
}

/// The move to send for one pointer position, or null when there is none.
///
/// While a button is held the browser keeps sending moves after the pointer
/// has left the picture (pointer capture), and the host's drag loop needs
/// them: a gap makes Windows end the drag. Clamping to the edge is the
/// closest honest position, so the drag continues. With no button held a
/// move outside the picture has nowhere to put the remote cursor, so it is
/// dropped as before.
export function movePoint(
  nx: number,
  ny: number,
  buttonHeld: boolean,
): PicturePoint | null {
  if (!buttonHeld && (nx < 0 || nx > 1 || ny < 0 || ny > 1)) return null
  return clampPoint(nx, ny)
}

/// The `down:false` events for every button a gesture still holds. All
/// release paths (pointerup, cancel, lost capture, window blur) use the same
/// list so a button can never stay pressed on the host.
export function buttonReleaseEvents(
  buttons: Iterable<number>,
): { k: 'button'; button: number; down: false }[] {
  return [...buttons].map((button) => ({ k: 'button', button, down: false }))
}

/// The host buttons this page has pressed and not yet released.
///
/// `drain()` is the one way out: it empties the set and returns what still
/// needs a `down:false`, which makes every release path idempotent even when
/// a pointerup and the lost-capture notification both fire.
export class PressedButtons {
  private readonly buttons = new Set<number>()

  press(button: number): void {
    this.buttons.add(button)
  }

  get size(): number {
    return this.buttons.size
  }

  drain(): number[] {
    const held = [...this.buttons]
    this.buttons.clear()
    return held
  }
}
