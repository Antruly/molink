import assert from 'node:assert/strict'
import test from 'node:test'
import {
  DEFAULT_LAZY_BYTES,
  DEFAULT_LAZY_FILES,
  MAX_CLIPBOARD_FILE_BYTES,
  MAX_DIRECT_TOTAL_BYTES,
  directUploadEligible,
  lazyCapability,
  lazyOfferEligible,
} from '../src/lib/clipboard-lazy.ts'

const MiB = 1024 * 1024

test('a host without clipboard_lazy has no capability', () => {
  assert.equal(lazyCapability(null), null)
  assert.equal(lazyCapability(undefined), null)
  assert.equal(lazyCapability({}), null)
  assert.equal(lazyCapability({ clipboard_lazy: false }), null)
  assert.equal(lazyCapability({ clipboard_lazy: 'true' }), null)
})

test('the welcome limits are used when they are sane', () => {
  assert.deepEqual(lazyCapability({ clipboard_lazy: true }), {
    maxFiles: DEFAULT_LAZY_FILES,
    maxBytes: DEFAULT_LAZY_BYTES,
    maxFileBytes: MAX_CLIPBOARD_FILE_BYTES,
    stream: false,
  })
  assert.deepEqual(
    lazyCapability({
      clipboard_lazy: true,
      clipboard_lazy_files: 3,
      clipboard_lazy_bytes: 1 * 1024 * 1024,
    }),
    {
      maxFiles: 3,
      maxBytes: 1024 * 1024,
      maxFileBytes: 1024 * 1024,
      stream: false,
    },
  )
})

test('a malformed limit falls back instead of rejecting everything', () => {
  assert.deepEqual(
    lazyCapability({
      clipboard_lazy: true,
      clipboard_lazy_files: 0,
      clipboard_lazy_bytes: -5,
    }),
    {
      maxFiles: DEFAULT_LAZY_FILES,
      maxBytes: DEFAULT_LAZY_BYTES,
      maxFileBytes: MAX_CLIPBOARD_FILE_BYTES,
      stream: false,
    },
  )
})

test('eight files within the caps are eligible', () => {
  const capability = lazyCapability({ clipboard_lazy: true })
  const files = Array.from({ length: 8 }, () => ({ size: 50 * MiB }))
  assert.equal(lazyOfferEligible(files, capability), true)
})

test('a ninth file is not eligible', () => {
  const capability = lazyCapability({ clipboard_lazy: true })
  const files = Array.from({ length: 9 }, () => ({ size: 1 }))
  assert.equal(lazyOfferEligible(files, capability), false)
})

test('the total budget and the single-file cap are both enforced', () => {
  const capability = lazyCapability({ clipboard_lazy: true })
  assert.equal(
    lazyOfferEligible([{ size: DEFAULT_LAZY_BYTES }], capability),
    false,
    'one file may not exceed the local core cap',
  )
  assert.equal(
    lazyOfferEligible([{ size: 100 }, { size: 100 }], {
      maxFiles: 8,
      maxBytes: 150,
      maxFileBytes: MAX_CLIPBOARD_FILE_BYTES,
    }),
    false,
    'the advertised total budget is enforced',
  )
  assert.equal(lazyOfferEligible([], capability), false)
  assert.equal(
    lazyOfferEligible([{ size: 1 }], null),
    false,
    'an incapable host never takes the lazy path',
  )
})

test('the eager fallback still has its own 256 MiB total limit', () => {
  assert.equal(directUploadEligible([{ size: 100 * MiB }]), true)
  assert.equal(directUploadEligible([{ size: MAX_DIRECT_TOTAL_BYTES }]), true)
  assert.equal(
    directUploadEligible([{ size: MAX_DIRECT_TOTAL_BYTES + 1 }]),
    false,
  )
  assert.equal(
    directUploadEligible([{ size: 200 * MiB }, { size: 100 * MiB }]),
    false,
  )
  assert.equal(directUploadEligible([]), false)
})
