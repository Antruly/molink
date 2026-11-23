import assert from 'node:assert/strict'
import test from 'node:test'
import {
  directoryPickMode,
  directoryPickNoteKey,
} from '../src/lib/directory-pick.ts'

test('a secure context with the File System Access API uses it first', () => {
  assert.equal(
    directoryPickMode({
      secureContext: true,
      showDirectoryPicker: () => undefined,
      shellBridge: true,
      supportsDirectoryInput: true,
    }),
    'fs-access',
  )
  // The API exists but the page is plain http: calling it throws, so it must
  // not be chosen.
  assert.equal(
    directoryPickMode({
      secureContext: false,
      showDirectoryPicker: () => undefined,
      shellBridge: false,
      supportsDirectoryInput: false,
    }),
    'unsupported',
  )
})

test('the WebKit desktop shell opens its native folder dialog', () => {
  // WebKitGTK has no showDirectoryPicker and degrades webkitdirectory to a
  // file picker; the shell bridge is the only correct option.
  assert.equal(
    directoryPickMode({
      secureContext: false,
      shellBridge: true,
      supportsDirectoryInput: false,
    }),
    'shell',
  )
})

test('an insecure Chromium keeps the webkitdirectory folder chooser', () => {
  assert.equal(
    directoryPickMode({
      secureContext: false,
      shellBridge: false,
      supportsDirectoryInput: true,
    }),
    'directory-input',
  )
})

test('a plain browser with neither option falls back to a message, not a file picker', () => {
  const mode = directoryPickMode({
    secureContext: false,
    shellBridge: false,
    supportsDirectoryInput: false,
  })
  assert.equal(mode, 'unsupported')
  assert.equal(directoryPickNoteKey(mode), 'files.dirPickerUnsupported')
  assert.equal(
    directoryPickNoteKey('shell'),
    'files.dirPickerShell',
  )
  assert.equal(directoryPickNoteKey('directory-input'),
    'files.dirPickerInputFallback')
  assert.equal(directoryPickNoteKey('fs-access'), null)
})
