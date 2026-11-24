// Recovery step of the viewer's load controller.
//
// Run with: npm test  (Node's built-in test runner strips the TypeScript
// types of the imported module without a build step).
import test from 'node:test'
import assert from 'node:assert/strict'

import { fpsPlan, nextFpsUp } from './frame-rate.ts'

test('recovery is proportional, not a fixed +5 jump', () => {
  const plan = fpsPlan('auto', 'turbo') // min 5, max 25
  assert.equal(nextFpsUp(5, plan), 6)
  assert.equal(nextFpsUp(10, plan), 12)
})

test('recovery never exceeds the plan ceiling', () => {
  const plan = fpsPlan(15, 'quality') // fixed 15
  assert.equal(nextFpsUp(14, plan), 15)
  assert.equal(nextFpsUp(15, plan), 15)
})

test('recovery never exceeds the encoder ceiling', () => {
  const plan = fpsPlan('auto', 'balanced')
  assert.equal(nextFpsUp(8, plan, 9), 9)
  // An encoder that cannot sustain even one more frame keeps the target.
  assert.equal(nextFpsUp(8, plan, 7), 7)
})

test('auto no longer oscillates between 5 and 10', () => {
  const plan = fpsPlan('auto', 'balanced')
  let target = 5
  const climbed = []
  for (let i = 0; i < 6; i += 1) {
    const next = nextFpsUp(target, plan)
    climbed.push(next)
    if (next <= target) break
    target = next
  }
  assert.deepEqual(climbed, [6, 7, 8, 9, 10, 12])
  // None of the steps is the old +5 jump.
  for (let i = 1; i < climbed.length; i += 1) {
    assert.ok(climbed[i] - climbed[i - 1] <= 2)
  }
})
