// Secure-context rules for the two directory pickers.
import test from 'node:test'
import assert from 'node:assert/strict'

import {
  canPickDirectory,
  canUseFileSystemAccess,
  supportsDirectoryInput,
} from './download-save.ts'

test('showDirectoryPicker is only used on a secure context', () => {
  const secure = {
    isSecureContext: true,
    showSaveFilePicker() {},
    showDirectoryPicker() {},
  }
  const insecure = { ...secure, isSecureContext: false }
  assert.equal(canUseFileSystemAccess(secure), true)
  assert.equal(canPickDirectory(secure), true)
  // http://192.168.x.x: calling it throws SecurityError, so never try.
  assert.equal(canUseFileSystemAccess(insecure), false)
  assert.equal(canPickDirectory(insecure), false)
})

test('the folder input fallback is detected instead of assumed', () => {
  assert.equal(
    supportsDirectoryInput({ createElement: () => ({ webkitdirectory: '' }) }),
    true,
  )
  assert.equal(supportsDirectoryInput({ createElement: () => ({}) }), false)
  assert.equal(supportsDirectoryInput({}), false)
  assert.equal(
    supportsDirectoryInput({
      createElement: () => {
        throw new Error('no DOM')
      },
    }),
    false,
  )
})
