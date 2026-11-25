import assert from 'node:assert/strict'
import test from 'node:test'
import {
  acceptDownloadChunk,
  cancelDownload,
  downloadComplete,
  downloadSinkFor,
  downloadStartDetail,
  eofOutcome,
  loadDownloadOffset,
  saveDownloadOffset,
} from '../src/lib/download-state.ts'

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

test('cancelling a download rejects chunks still in flight', () => {
  const progress = { size: 8, received: 4, eofSeen: false }
  cancelDownload(progress)
  assert.equal(acceptDownloadChunk(progress, 4, 4), false)
  assert.equal(progress.received, 4)
  assert.equal(downloadComplete(progress), false)
  progress.eofSeen = true
  assert.equal(downloadComplete(progress), false)
})

test('the sink is the target that actually exists', () => {
  assert.equal(
    downloadSinkFor({ stagingFile: true, writable: true }),
    'chunk',
  )
  assert.equal(downloadSinkFor({ stagingFile: false, writable: true }), 'fs')
  assert.equal(downloadSinkFor({ stagingFile: false, writable: false }), 'blob')
})

test('eof waits for a final chunk in flight, then fails honestly', () => {
  const progress = { size: 8, received: 4, eofSeen: true }
  const eofAt = 1_000
  // Control is drained before file bytes: eof may arrive before the chunk.
  assert.equal(eofOutcome(progress, eofAt, 1_001, 3_000), 'waiting')
  assert.equal(eofOutcome(progress, eofAt, 3_999, 3_000), 'waiting')
  assert.equal(eofOutcome(progress, eofAt, 4_000, 3_000), 'incomplete')
  // The late chunk completes the transfer before the grace expires.
  progress.received = 8
  assert.equal(eofOutcome(progress, eofAt, 1_500, 3_000), 'complete')
  assert.equal(eofOutcome(progress, eofAt, 9_000, 3_000), 'complete')
})

test('an abandoned download is never completed by a late eof', () => {
  const progress = { size: 8, received: 8, eofSeen: true, cancelled: true }
  assert.equal(eofOutcome(progress, 0, 10_000, 3_000), 'incomplete')
})

test('the start line is bounded and names the sink', () => {
  assert.equal(
    downloadStartDetail('chunk', 'a b.txt', 123),
    'mode=chunk name=a b.txt size=123',
  )
  const long = 'x'.repeat(200)
  const detail = downloadStartDetail('blob', long, 7)
  assert.equal(detail.startsWith('mode=blob name='), true)
  assert.equal(detail.endsWith(' size=7'), true)
  assert.equal(detail.includes('\n'), false)
  assert.equal(
    downloadStartDetail('fs', 'line\r\nname', 1),
    'mode=fs name=line??name size=1',
  )
})

test('a staged offset round-trips and unusable values are ignored', () => {
  const map = new Map()
  const store = {
    getItem: (key) => map.get(key) ?? null,
    setItem: (key, value) => map.set(key, value),
    removeItem: (key) => map.delete(key),
  }
  saveDownloadOffset(store, 'k', 5, 10)
  assert.equal(loadDownloadOffset(store, 'k', 10), 5)
  // Zero and a complete file clear the entry instead of storing it.
  saveDownloadOffset(store, 'k', 0, 10)
  assert.equal(loadDownloadOffset(store, 'k', 10), 0)
  saveDownloadOffset(store, 'k', 10, 10)
  assert.equal(map.has('k'), false)
  // A remembered offset larger than the file is not a resume point.
  map.set('k', '11')
  assert.equal(loadDownloadOffset(store, 'k', 10), 0)
  map.set('k', 'nope')
  assert.equal(loadDownloadOffset(store, 'k', 10), 0)
})
