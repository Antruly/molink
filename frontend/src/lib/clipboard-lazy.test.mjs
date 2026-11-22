// Unit tests for the pure clipboard-stream helpers.
//
// Run with: npm test  (Node's built-in test runner; Node 24 strips the
// TypeScript types of the imported module without a build step).
import test from 'node:test'
import assert from 'node:assert/strict'

import {
  MAX_CLIPBOARD_RANGE,
  clipboardFileMode,
  clipboardRangeQuery,
  resolveClipboardFetch,
} from './clipboard-lazy.ts'

const capability = (stream) => ({
  maxFiles: 8,
  maxBytes: 2 * 1024 * 1024 * 1024,
  maxFileBytes: 256 * 1024 * 1024,
  stream,
})

test('a streaming capability offers descriptors without uploading', () => {
  const files = [{ name: 'a.bin', size: 10 }]
  assert.equal(clipboardFileMode(files, capability(true)), 'stream')
  assert.equal(clipboardFileMode(files, capability(false)), 'lazy')
  // No host capability: the eager path is still the only option.
  assert.equal(clipboardFileMode(files, null), 'direct')
  // Outside the lazy limits and the eager limit: nothing can be synced.
  assert.equal(
    clipboardFileMode([{ name: 'huge.bin', size: 2 ** 40 }], capability(true)),
    'none',
  )
})

test('a range query carries offset and length', () => {
  assert.equal(clipboardRangeQuery(65536, 65536), '&offset=65536&length=65536')
  assert.equal(clipboardRangeQuery(), '')
  assert.equal(clipboardRangeQuery(0), '&offset=0')
})

test('a valid fetch resolves to the offered file', () => {
  const files = [
    { id: 'id-a', name: 'a.bin', size: 100 },
    { id: 'id-b', name: 'b.bin', size: 200 },
  ]
  const result = resolveClipboardFetch(
    { id: 7, index: 1, name: 'b.bin', offset: 64, length: 32 },
    files,
  )
  assert.equal(result.ok, true)
  assert.deepEqual(result.target, {
    fileIndex: 1,
    fileId: 'id-b',
    offset: 64,
    length: 32,
    size: 200,
  })
  // Without an index the name still identifies the file.
  const byName = resolveClipboardFetch(
    { id: 8, name: 'a.bin', offset: 0, length: 16 },
    files,
  )
  assert.equal(byName.ok, true)
  assert.equal(byName.target.fileId, 'id-a')
})

test('a bad or stale fetch is answered with an error', () => {
  const files = [{ id: 'id-a', name: 'a.bin', size: 100 }]
  const cases = [
    { id: 0, offset: 0, length: 1 }, // no request id
    { id: 1, offset: 0, length: 0 }, // no bytes
    { id: 1, offset: 0, length: MAX_CLIPBOARD_RANGE + 1 },
    { id: 1, index: 9, offset: 0, length: 1 }, // no such file
    { id: 1, name: 'gone.bin', offset: 0, length: 1 }, // stale offer
    { id: 1, name: 'a.bin', offset: 101, length: 1 }, // past EOF
  ]
  for (const message of cases) {
    const result = resolveClipboardFetch(message, files)
    assert.equal(result.ok, false, JSON.stringify(message))
    assert.equal(result.error, 1)
  }
  // A file without a local clipboard id cannot be read.
  const noId = resolveClipboardFetch(
    { id: 1, index: 0, offset: 0, length: 1 },
    [{ name: 'a.bin', size: 100 }],
  )
  assert.equal(noId.ok, false)
})

test('a short range at end of file is still a valid request', () => {
  const result = resolveClipboardFetch(
    { id: 3, index: 0, offset: 90, length: 64 },
    [{ id: 'id-a', name: 'a.bin', size: 100 }],
  )
  assert.equal(result.ok, true)
  assert.equal(result.target.offset, 90)
  assert.equal(result.target.length, 64)
})
