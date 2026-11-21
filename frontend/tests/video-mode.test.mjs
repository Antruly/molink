import assert from 'node:assert/strict'
import test from 'node:test'
import {
  DEFAULT_VIDEO_MODE,
  VIDEO_MODE_OPTIONS,
  normalizeVideoMode,
} from '../src/lib/video-mode.ts'

test('the selector defaults to auto', () => {
  assert.equal(DEFAULT_VIDEO_MODE, 'auto')
  assert.equal(VIDEO_MODE_OPTIONS[0], 'auto')
  assert.deepEqual([...VIDEO_MODE_OPTIONS], ['auto', 'tiles', 'stream'])
})

test('the legacy mixed value selects auto', () => {
  assert.equal(normalizeVideoMode('mixed'), 'auto')
})

test('unknown or missing values select auto', () => {
  for (const value of [undefined, null, '', 'garbage', 42]) {
    assert.equal(normalizeVideoMode(value), 'auto')
  }
})

test('explicit paths are preserved', () => {
  assert.equal(normalizeVideoMode('tiles'), 'tiles')
  assert.equal(normalizeVideoMode('stream'), 'stream')
  assert.equal(normalizeVideoMode('auto'), 'auto')
})
