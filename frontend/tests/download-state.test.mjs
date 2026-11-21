import assert from 'node:assert/strict'
import test from 'node:test'
import { acceptDownloadChunk, downloadComplete } from '../src/lib/download-state.ts'

test('an early EOF waits for the final file bytes', () => {
  const progress = { size: 8, received: 4, eofSeen: true }
  assert.equal(downloadComplete(progress), false)
  assert.equal(acceptDownloadChunk(progress, 4, 4), true)
  assert.equal(downloadComplete(progress), true)
})

test('all bytes still require EOF, including an empty download', () => {
  for (const size of [0, 4]) {
    const progress = { size, received: size, eofSeen: false }
    assert.equal(downloadComplete(progress), false)
    progress.eofSeen = true
    assert.equal(downloadComplete(progress), true)
  }
})

test('duplicates, gaps and oversized chunks do not advance the offset', () => {
  const progress = { size: 8, received: 4, eofSeen: true }
  for (const [offset, length] of [[0, 4], [5, 3], [4, 5], [4, 0]]) {
    assert.equal(acceptDownloadChunk(progress, offset, length), false)
    assert.equal(progress.received, 4)
    assert.equal(downloadComplete(progress), false)
  }
})
