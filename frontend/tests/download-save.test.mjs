import assert from 'node:assert/strict'
import test from 'node:test'
import {
  basename,
  canPickDirectory,
  canUseFileSystemAccess,
  isPickerCancellation,
  maySaveFromButton,
  mustAbortStream,
  saveStrategyFor,
  suggestedFileName,
} from '../src/lib/download-save.ts'

const api = { showSaveFilePicker: () => undefined }

test('the picker is only chosen on a secure context that exposes it', () => {
  assert.equal(canUseFileSystemAccess({ ...api, isSecureContext: true }), true)
  assert.equal(canUseFileSystemAccess({ ...api, isSecureContext: false }), false)
  assert.equal(canUseFileSystemAccess({ isSecureContext: true }), false)
  assert.equal(saveStrategyFor({ ...api, isSecureContext: true }), 'picker')
  assert.equal(saveStrategyFor({ ...api, isSecureContext: false }), 'button')
  assert.equal(saveStrategyFor({ isSecureContext: true }), 'button')
})

test('a directory picker needs a secure context and the API itself', () => {
  const picker = { showDirectoryPicker: () => undefined }
  assert.equal(canPickDirectory({ ...picker, isSecureContext: true }), true)
  assert.equal(canPickDirectory({ ...picker, isSecureContext: false }), false)
  assert.equal(canPickDirectory({ isSecureContext: true }), false)
  assert.equal(
    canPickDirectory({ showDirectoryPicker: 'yes', isSecureContext: true }),
    false,
  )
})

test('cancelling the picker is not a transfer failure', () => {
  assert.equal(isPickerCancellation({ name: 'AbortError' }), true)
  assert.equal(isPickerCancellation({ name: 'NotAllowedError' }), false)
  assert.equal(isPickerCancellation(null), false)
  assert.equal(isPickerCancellation(new Error('AbortError')), false)
})

test('a Save button is offered only for a complete, un-abandoned download', () => {
  const complete = { size: 8, received: 8, eofSeen: true }
  assert.equal(maySaveFromButton('button', complete, false), true)
  assert.equal(maySaveFromButton('button', complete, true), false)
  assert.equal(maySaveFromButton('picker', complete, false), false)
  assert.equal(
    maySaveFromButton('button', { size: 8, received: 7, eofSeen: true }, false),
    false,
  )
  assert.equal(
    maySaveFromButton('button', { size: 8, received: 8, eofSeen: false }, false),
    false,
  )
  assert.equal(
    maySaveFromButton(
      'button',
      { size: 8, received: 8, eofSeen: true, cancelled: true },
      false,
    ),
    false,
  )
})

test('a streamed file is aborted unless it was committed', () => {
  assert.equal(mustAbortStream('picker', false), true)
  assert.equal(mustAbortStream('picker', true), false)
  assert.equal(mustAbortStream('button', false), false)
})

test('host paths yield clean suggested names', () => {
  assert.equal(basename('/home/u/a.txt'), 'a.txt')
  assert.equal(basename('C:\\Users\\u\\a.txt'), 'a.txt')
  assert.equal(basename('/tmp/dir/'), 'dir')
  assert.equal(suggestedFileName('', '/home/u/report.pdf'), 'report.pdf')
  assert.equal(suggestedFileName('a/b:c.txt', '/x'), 'a_b_c.txt')
  assert.equal(suggestedFileName('   ', '/x/name.bin'), 'name.bin')
})
