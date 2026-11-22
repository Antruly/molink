import assert from 'node:assert/strict'
import test from 'node:test'
import {
  clipboardFilesMode,
  isLocalCorePage,
  normalizeOrigin,
  stageableClipboardFile,
} from '../src/lib/clipboard-local.ts'

test('a remote page never counts as the local core', () => {
  assert.equal(
    isLocalCorePage(false, 'http://192.168.2.20:28418',
                   'http://192.168.2.20:28418'),
    false,
  )
  assert.equal(
    clipboardFilesMode(false, 'http://192.168.2.20:28418',
                       'http://192.168.2.20:28418', true),
    'local-app',
  )
})

test('a loopback page served by the local core is same-origin', () => {
  assert.equal(
    isLocalCorePage(true, 'http://127.0.0.1:28418',
                    'http://127.0.0.1:28418'),
    true,
  )
  assert.equal(
    clipboardFilesMode(true, 'http://127.0.0.1:28418',
                       'http://127.0.0.1:28418', true),
    'auto',
  )
  assert.equal(
    clipboardFilesMode(true, 'http://127.0.0.1:28418',
                       'http://127.0.0.1:28418', false),
    'manual',
  )
})

test('the Vite dev origin is the one allowed cross-origin page', () => {
  assert.equal(
    clipboardFilesMode(true, 'http://localhost:1420',
                       'http://127.0.0.1:28418', true),
    'auto',
  )
  assert.equal(
    clipboardFilesMode(true, 'http://192.168.1.9:1420',
                       'http://127.0.0.1:28418', true),
    'local-app',
  )
})

test('origins are compared after normalisation', () => {
  assert.equal(normalizeOrigin('http://127.0.0.1:28418/'), 'http://127.0.0.1:28418')
  assert.equal(
    clipboardFilesMode(true, 'http://127.0.0.1:28418/',
                       'http://127.0.0.1:28418', true),
    'auto',
  )
})

test('only non-empty, safe sizes are staged automatically', () => {
  assert.equal(stageableClipboardFile({ size: 5 }), true)
  assert.equal(stageableClipboardFile({ size: 0 }), false)
  assert.equal(stageableClipboardFile({ size: -1 }), false)
  assert.equal(stageableClipboardFile({ size: 1.5 }), false)
  assert.equal(stageableClipboardFile({}), false)
})
