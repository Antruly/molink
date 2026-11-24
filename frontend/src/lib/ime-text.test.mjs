// The host accepts at most 4096 UTF-8 bytes per `{k:"text"}` event. A long
// IME commit is split, never rejected, and the split must not cut a code
// point in half.
import assert from 'node:assert/strict'
import test from 'node:test'

import {
  INPUT_TEXT_MAX_BYTES,
  splitImeText,
  utf8ByteLength,
} from './ime-text.ts'

test('empty text produces no events', () => {
  assert.deepEqual(splitImeText(''), [])
})

test('text under the limit is one chunk', () => {
  const text = '中文测试'
  assert.equal(utf8ByteLength(text), 12)
  assert.deepEqual(splitImeText(text), [text])
})

test('a chunk of exactly 4096 bytes stays whole', () => {
  const text = 'a'.repeat(INPUT_TEXT_MAX_BYTES)
  const chunks = splitImeText(text)
  assert.equal(chunks.length, 1)
  assert.equal(chunks[0], text)
})

test('one byte over the limit becomes two chunks', () => {
  const text = 'a'.repeat(INPUT_TEXT_MAX_BYTES + 1)
  const chunks = splitImeText(text)
  assert.equal(chunks.length, 2)
  assert.equal(chunks[0].length, INPUT_TEXT_MAX_BYTES)
  assert.equal(chunks[1].length, 1)
  assert.equal(chunks.join(''), text)
})

test('Chinese text is split on code points and reassembles', () => {
  const text = '中'.repeat(2000) // 6000 UTF-8 bytes
  const chunks = splitImeText(text)
  assert.equal(chunks.length, 2)
  assert.equal(utf8ByteLength(chunks[0]), 4095)
  assert.equal(utf8ByteLength(chunks[1]), 1905)
  assert.equal(chunks.join(''), text)
  for (const chunk of chunks) {
    assert.ok(utf8ByteLength(chunk) <= INPUT_TEXT_MAX_BYTES)
    // Every chunk must be valid UTF-8: encoding it back and decoding it
    // yields the same string (no split surrogate and no broken sequence).
    assert.equal(new TextDecoder().decode(new TextEncoder().encode(chunk)), chunk)
  }
})

test('emoji surrogate pairs are never split', () => {
  const text = '😀'.repeat(1025) // 4 bytes each: 4100 bytes
  const chunks = splitImeText(text)
  assert.equal(chunks.length, 2)
  assert.equal(utf8ByteLength(chunks[0]), 4096)
  assert.equal(utf8ByteLength(chunks[1]), 4)
  assert.equal(chunks.join(''), text)
  for (const chunk of chunks) {
    // An odd number of UTF-16 units would mean a split surrogate pair.
    assert.equal(chunk.length % 2, 0)
  }
})

test('mixed text keeps each chunk within the byte and code-point limits', () => {
  const text = ('中a😀'.repeat(1000)) // 3 + 1 + 4 = 8 bytes per group
  const chunks = splitImeText(text)
  assert.equal(chunks.join(''), text)
  for (const chunk of chunks) {
    assert.ok(utf8ByteLength(chunk) <= INPUT_TEXT_MAX_BYTES)
  }
})
