import assert from 'node:assert/strict'
import test from 'node:test'
import {
  createWheelAccumulator,
  remoteButton,
  wheelDeltaToNotches,
  wheelNotches,
} from './pointer.ts'

test('browser buttons 3 and 4 map to the host back/forward buttons', () => {
  assert.equal(remoteButton(0), 1)
  assert.equal(remoteButton(1), 2)
  assert.equal(remoteButton(2), 3)
  assert.equal(remoteButton(3), 4)
  assert.equal(remoteButton(4), 5)
  assert.equal(remoteButton(-1), null)
  assert.equal(remoteButton(5), null)
  assert.equal(remoteButton(1.5), null)
})

test('delta units follow deltaMode', () => {
  assert.equal(wheelDeltaToNotches(3, 0), 0.03)
  assert.equal(wheelDeltaToNotches(100, 0), 1)
  assert.equal(wheelDeltaToNotches(2, 1), 2)
  assert.equal(wheelDeltaToNotches(1, 2), 3)
  assert.equal(wheelDeltaToNotches(Number.NaN, 0), 0)
})

test('a slow trackpad accumulates to a notch instead of scrolling 34 times', () => {
  const accumulator = createWheelAccumulator()
  let notches = 0
  for (let i = 0; i < 33; i += 1) {
    const step = wheelNotches(accumulator, { deltaY: 3 })
    assert.equal(step.dx, 0)
    assert.equal(step.dy, 0, `3 px x ${i + 1} is still below one notch`)
    notches += step.dy
  }
  const last = wheelNotches(accumulator, { deltaY: 3 })
  assert.equal(last.dy, 1, 'the 34th 3 px step reaches one notch')
  notches += last.dy
  assert.equal(notches, 1)
  // The remainder stays for the next notch: 102 px leaves 2 px.
  assert.ok(Math.abs(accumulator.y - 0.02) < 1e-9)
})

test('a 120 px wheel notch is sent once and its remainder is kept', () => {
  const accumulator = createWheelAccumulator()
  assert.deepEqual(wheelNotches(accumulator, { deltaY: 120 }), { dx: 0, dy: 1 })
  assert.deepEqual(wheelNotches(accumulator, { deltaY: 120 }), { dx: 0, dy: 1 })
  assert.deepEqual(wheelNotches(accumulator, { deltaY: 120 }), { dx: 0, dy: 1 })
  // Three notches sent; 0.6 of a notch is left, so 60 px more fires another.
  assert.deepEqual(wheelNotches(accumulator, { deltaY: 60 }), { dx: 0, dy: 1 })
  assert.ok(Math.abs(accumulator.y - 0.2) < 1e-9)
})

test('line and page deltas are whole notches', () => {
  const accumulator = createWheelAccumulator()
  assert.deepEqual(
    wheelNotches(accumulator, { deltaY: 2, deltaMode: 1 }),
    { dx: 0, dy: 2 },
  )
  assert.deepEqual(
    wheelNotches(accumulator, { deltaY: 1, deltaMode: 2 }),
    { dx: 0, dy: 3 },
  )
  assert.deepEqual(
    wheelNotches(accumulator, { deltaY: -1, deltaMode: 1 }),
    { dx: 0, dy: -1 },
  )
})

test('horizontal deltas use dx', () => {
  const accumulator = createWheelAccumulator()
  assert.deepEqual(
    wheelNotches(accumulator, { deltaX: 120 }),
    { dx: 1, dy: 0 },
  )
  assert.deepEqual(
    wheelNotches(accumulator, { deltaX: -120 }),
    { dx: -1, dy: 0 },
  )
})

test('shift+wheel scrolls horizontally', () => {
  const accumulator = createWheelAccumulator()
  assert.deepEqual(
    wheelNotches(accumulator, { deltaY: 120, shiftKey: true }),
    { dx: 1, dy: 0 },
  )
  assert.deepEqual(
    wheelNotches(accumulator, { deltaY: -120, shiftKey: true }),
    { dx: -1, dy: 0 },
  )
  assert.equal(accumulator.y, 0, 'no vertical remainder is left behind')
})

test('negative trackpad deltas accumulate without losing the sign', () => {
  const accumulator = createWheelAccumulator()
  assert.deepEqual(wheelNotches(accumulator, { deltaY: -3 }), { dx: 0, dy: 0 })
  assert.deepEqual(wheelNotches(accumulator, { deltaY: -3 }), { dx: 0, dy: 0 })
  assert.deepEqual(wheelNotches(accumulator, { deltaY: -120 }), { dx: 0, dy: -1 })
})
