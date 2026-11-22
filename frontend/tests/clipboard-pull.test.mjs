import assert from 'node:assert/strict'
import test from 'node:test'
import {
  PULL_CHUNK_BYTES,
  PULL_MAX_FILES,
  PULL_MAX_FILE_BYTES,
  batchKeyMaterial,
  checkPullChunk,
  clipboardBatchId,
  fallbackBatchId,
  planPullFiles,
  pullChunkLength,
} from '../src/lib/clipboard-pull.ts'

const MiB = 1024 * 1024

test('valid pull descriptors survive, junk is ignored', () => {
  const plan = planPullFiles([
    { id: 1, name: 'a.txt', size: 3, path: 'C:\\x\\a.txt' },
    { id: 0, name: 'bad.txt', size: 1 },
    { id: 2, name: '', size: 1 },
    { id: 3, name: 'c.txt', size: -1 },
    { id: 4, name: 'd.txt', size: 1.5 },
    { id: 5, name: 'e.txt', size: 5 },
  ])
  assert.deepEqual(plan.files, [
    { id: 1, name: 'a.txt', size: 3, path: 'C:\\x\\a.txt' },
    { id: 5, name: 'e.txt', size: 5 },
  ])
  // Invalid descriptors are ignored rather than counted as a truncated sync.
  assert.equal(plan.omitted, 0)
})

test('the plan keeps a prefix instead of dropping the whole selection', () => {
  const files = Array.from({ length: PULL_MAX_FILES + 3 }, (_, index) => ({
    id: index + 1,
    name: `${index}.bin`,
    size: 1,
  }))
  files.splice(2, 0, {
    id: 99,
    name: 'huge.bin',
    size: PULL_MAX_FILE_BYTES + 1,
  })
  const plan = planPullFiles(files)
  assert.equal(plan.files.length, 2)
  assert.equal(plan.omitted, files.length - 2)
  assert.equal(plan.files[1].name, '1.bin')
})

test('zero-byte files are valid and complete without a request', () => {
  const plan = planPullFiles([{ id: 1, name: 'empty.txt', size: 0 }])
  assert.equal(plan.files.length, 1)
  assert.equal(pullChunkLength(0, 0), 0)
  assert.equal(pullChunkLength(10, 0), 10)
  assert.equal(pullChunkLength(PULL_CHUNK_BYTES * 2, 0), PULL_CHUNK_BYTES)
  assert.equal(pullChunkLength(10, 10), 0)
})

test('a chunk reply is checked before it is written', () => {
  assert.deepEqual(checkPullChunk(0, 0, 0, 4, 4), { ok: true })
  assert.deepEqual(checkPullChunk(2, 0, 0, 4, 0), {
    ok: false,
    reason: 'gone',
  })
  assert.deepEqual(checkPullChunk(1, 0, 0, 4, 0), {
    ok: false,
    reason: 'failed',
  })
  // A short local read must not be mistaken for end-of-file.
  assert.deepEqual(checkPullChunk(0, 0, 0, 4, 3), {
    ok: false,
    reason: 'incomplete',
  })
  assert.deepEqual(checkPullChunk(0, 4, 0, 4, 4), {
    ok: false,
    reason: 'incomplete',
  })
})

test('the batch id has the shape the staging route accepts', async () => {
  const files = [{ id: 1, name: 'a.txt', size: 3 }]
  const id = await clipboardBatchId(files, 'nonce')
  assert.match(id, /^[0-9a-f]{8,64}$/)
  assert.equal(id.length, 40)
  // The material includes a per-copy nonce, so copying again gets a new dir.
  assert.notEqual(
    batchKeyMaterial(files, 'a'),
    batchKeyMaterial(files, 'b'),
  )
  // No SubtleCrypto (an insecure page or a test process): still hex.
  const fallback = await clipboardBatchId(files, 'nonce', null)
  assert.match(fallback, /^[0-9a-f]{8,64}$/)
  assert.equal(fallback, fallbackBatchId(batchKeyMaterial(files, 'nonce')))
})

test('two identical batches differ by nonce', async () => {
  const files = [{ id: 7, name: 'a.txt', size: 3 }]
  const first = await clipboardBatchId(files, 'one')
  const second = await clipboardBatchId(files, 'two')
  assert.notEqual(first, second)
})
