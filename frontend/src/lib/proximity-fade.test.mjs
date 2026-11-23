// Geometry behind the "fade overlays when the pointer comes close" rule.
import test from 'node:test'
import assert from 'node:assert/strict'

import {
  DEFAULT_FADE_RADIUS,
  fadedTargets,
  isNearRect,
} from './proximity-fade.ts'

const rect = { left: 100, top: 100, right: 200, bottom: 200 }

test('the default radius is 28px', () => {
  assert.equal(DEFAULT_FADE_RADIUS, 28)
})

test('a pointer inside the rectangle always hits', () => {
  assert.equal(isNearRect(150, 150, rect), true)
  assert.equal(isNearRect(100, 100, rect), true)
  assert.equal(isNearRect(200, 200, rect), true)
})

test('the radius expands every edge, boundaries inclusive', () => {
  // 28px left of the rect.
  assert.equal(isNearRect(72, 150, rect, 28), true)
  assert.equal(isNearRect(71.999, 150, rect, 28), false)
  // 28px right of the rect.
  assert.equal(isNearRect(228, 150, rect, 28), true)
  assert.equal(isNearRect(228.001, 150, rect, 28), false)
  // Above and below.
  assert.equal(isNearRect(150, 72, rect, 28), true)
  assert.equal(isNearRect(150, 71.999, rect, 28), false)
  assert.equal(isNearRect(150, 228, rect, 28), true)
  assert.equal(isNearRect(150, 228.001, rect, 28), false)
})

test('the expanded region is a rectangle, so corner gaps count as near', () => {
  // Both offsets are within the radius, so the square corner is included.
  assert.equal(isNearRect(72, 72, rect, 28), true)
  // One offset beyond the radius is out even though the diagonal distance
  // from the corner is close.
  assert.equal(isNearRect(71, 71, rect, 28), false)
})

test('a zero or negative radius only accepts the rectangle itself', () => {
  assert.equal(isNearRect(99, 150, rect, 0), false)
  assert.equal(isNearRect(100, 150, rect, 0), true)
  assert.equal(isNearRect(99, 150, rect, -5), false)
  assert.equal(isNearRect(100, 150, rect, -5), true)
})

test('degenerate and non-finite input never hits', () => {
  const hidden = { left: 0, top: 0, right: 0, bottom: 0 }
  assert.equal(isNearRect(0, 0, hidden, 28), false)
  assert.equal(isNearRect(Number.NaN, 10, rect), false)
  assert.equal(isNearRect(10, Number.NaN, rect), false)
  assert.equal(
    isNearRect(10, 10, { left: 0, top: 0, right: 10, bottom: 10 }, Number.NaN),
    true,
  )
})

test('overlays fade independently', () => {
  const badges = { left: 10, top: 10, right: 110, bottom: 40 }
  const stats = { left: 500, top: 500, right: 700, bottom: 530 }
  const connecting = { left: 300, top: 200, right: 420, bottom: 240 }
  const rects = { badges, stats, connecting }

  // Near the corner badges only.
  assert.deepEqual(fadedTargets(120, 25, rects, 28), {
    badges: true,
    stats: false,
    connecting: false,
  })
  // Near the stats bar only.
  assert.deepEqual(fadedTargets(690, 545, rects, 28), {
    badges: false,
    stats: true,
    connecting: false,
  })
  // Near the connecting watermark only.
  assert.deepEqual(fadedTargets(330, 190, rects, 28), {
    badges: false,
    stats: false,
    connecting: true,
  })
  // Out in the middle of the picture: nothing fades.
  assert.deepEqual(fadedTargets(250, 300, rects, 28), {
    badges: false,
    stats: false,
    connecting: false,
  })
  // Two nearby overlays fade together, independently of the third.
  const close = {
    ...rects,
    stats: { left: 140, top: 10, right: 200, bottom: 40 },
  }
  assert.deepEqual(fadedTargets(130, 25, close, 28), {
    badges: true,
    stats: true,
    connecting: false,
  })
})

test('missing rectangles stay unfaded', () => {
  assert.deepEqual(
    fadedTargets(0, 0, { badges: rect, stats: null, connecting: undefined }),
    { badges: false, stats: false, connecting: false },
  )
})
