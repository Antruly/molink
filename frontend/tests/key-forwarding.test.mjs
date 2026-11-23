import assert from 'node:assert/strict'
import test from 'node:test'
import { shouldForwardKeys } from '../src/lib/key-combos.ts'

function state(mask) {
  return {
    hasFocus: (mask & 1) !== 0,
    visible: (mask & 2) !== 0,
    controlEnabled: (mask & 4) !== 0,
    localTarget: (mask & 8) !== 0,
    settingsOpen: (mask & 16) !== 0,
  }
}

test('all 32 focus/visibility/control combinations follow the gate', () => {
  for (let mask = 0; mask < 32; mask += 1) {
    const input = state(mask)
    const expected =
      input.hasFocus &&
      input.visible &&
      input.controlEnabled &&
      !input.localTarget &&
      !input.settingsOpen
    assert.equal(
      shouldForwardKeys(input),
      expected,
      JSON.stringify(input),
    )
  }
})

test('keys are forwarded only when the remote picture is the input target', () => {
  const ready = {
    hasFocus: true,
    visible: true,
    controlEnabled: true,
    localTarget: false,
    settingsOpen: false,
  }
  assert.equal(shouldForwardKeys(ready), true)
  assert.equal(shouldForwardKeys({ ...ready, hasFocus: false }), false)
  assert.equal(shouldForwardKeys({ ...ready, visible: false }), false)
  assert.equal(shouldForwardKeys({ ...ready, controlEnabled: false }), false)
  assert.equal(shouldForwardKeys({ ...ready, localTarget: true }), false)
  assert.equal(shouldForwardKeys({ ...ready, settingsOpen: true }), false)
  // The quality panel is portaled outside the remote surface; while it is
  // open its sliders and dropdowns must not type into the host.
  assert.equal(shouldForwardKeys({ ...ready, localPanelOpen: true }), false)
  assert.equal(shouldForwardKeys({ ...ready, localPanelOpen: false }), true)
})
