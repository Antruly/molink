// Keyboard forwarding: the Linux X11 grab mirrors "pointer over the remote
// picture" in the page so keys stop going remote when the shell released the
// grab. Other platforms never set the field, so their rules stay unchanged.
import test from 'node:test'
import assert from 'node:assert/strict'

import { shouldForwardKeys } from './key-combos.ts'

const focused = {
  hasFocus: true,
  visible: true,
  controlEnabled: true,
  localTarget: false,
  settingsOpen: false,
}

test('the pointer gate only blocks when the page says outside', () => {
  assert.equal(shouldForwardKeys({ ...focused }), true)
  assert.equal(shouldForwardKeys({ ...focused, remotePictureHover: true }), true)
  assert.equal(shouldForwardKeys({ ...focused, remotePictureHover: false }), false)
})

test('the existing local-target gates still win over the pointer', () => {
  assert.equal(
    shouldForwardKeys({
      ...focused,
      localTarget: true,
      remotePictureHover: true,
    }),
    false,
  )
  assert.equal(
    shouldForwardKeys({ ...focused, hasFocus: false, remotePictureHover: true }),
    false,
  )
})
