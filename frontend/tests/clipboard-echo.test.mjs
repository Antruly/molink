import assert from 'node:assert/strict'
import test from 'node:test'
import {
  ClipboardEcho,
  ECHO_MAX_ENTRIES,
  ECHO_STORAGE_KEY,
  ECHO_TTL_MS,
  echoOfferKey,
  echoPathKey,
} from '../src/lib/clipboard-echo.ts'

class MemoryStorage {
  map = new Map()
  getItem(key) {
    return this.map.has(key) ? this.map.get(key) : null
  }
  setItem(key, value) {
    this.map.set(key, String(value))
  }
  removeItem(key) {
    this.map.delete(key)
  }
}

function clock(start = 1_000_000) {
  let value = start
  return {
    now: () => value,
    advance: (ms) => { value += ms },
  }
}

test('path keys ignore case, separator style and trailing slashes', () => {
  assert.equal(echoPathKey('C:\\Users\\A\\File.txt'), 'c:/users/a/file.txt')
  assert.equal(echoPathKey('c:/users/a/file.txt/'), 'c:/users/a/file.txt')
  assert.equal(echoPathKey('C:\\\\Users\\\\A\\\\File.txt'), 'c:/users/a/file.txt')
  assert.equal(echoPathKey(' /a/b/ '), '/a/b')
})

test('a path written to the remote clipboard is suppressed when it comes back', () => {
  const storage = new MemoryStorage()
  const time = clock()
  const echo = new ClipboardEcho(storage, time.now)
  echo.rememberPaths(['C:\\Users\\A\\Project\\a.txt'])
  assert.equal(echo.suppressPath('c:/users/a/project/a.txt'), true)
  assert.equal(echo.suppressPath('C:\\Users\\A\\Project\\A.TXT'), true)
  assert.equal(echo.suppressPath('C:\\Users\\A\\Project\\b.txt'), false)
})

test('suppression ends after the two-minute TTL', () => {
  const storage = new MemoryStorage()
  const time = clock()
  const echo = new ClipboardEcho(storage, time.now)
  echo.rememberPaths(['C:\\a.txt'])
  time.advance(ECHO_TTL_MS - 1)
  assert.equal(echo.suppressPath('C:\\a.txt'), true)
  time.advance(1)
  assert.equal(echo.suppressPath('C:\\a.txt'), false)
})

test('the record survives a new instance sharing the storage', () => {
  const storage = new MemoryStorage()
  const time = clock()
  new ClipboardEcho(storage, time.now).rememberPaths(['C:\\a.txt'])
  const second = new ClipboardEcho(storage, time.now)
  assert.equal(second.suppressPath('c:/a.txt'), true)
})

test('at most 64 entries are kept, oldest first', () => {
  const storage = new MemoryStorage()
  const time = clock()
  const echo = new ClipboardEcho(storage, time.now)
  for (let i = 0; i < ECHO_MAX_ENTRIES + 6; i += 1) {
    echo.rememberPaths([`C:\\file-${i}.txt`])
    time.advance(10)
  }
  assert.equal(echo.suppressPath('C:\\file-0.txt'), false)
  assert.equal(echo.suppressPath(`C:\\file-${ECHO_MAX_ENTRIES + 5}.txt`), true)
  const stored = JSON.parse(storage.getItem(ECHO_STORAGE_KEY))
  assert.equal(stored.length, ECHO_MAX_ENTRIES)
})

test('lazy offers are recognised by name and size, not by host path', () => {
  const storage = new MemoryStorage()
  const time = clock()
  const echo = new ClipboardEcho(storage, time.now)
  echo.rememberFiles([{ name: 'photo.jpg', size: 4096 }])
  assert.equal(
    echo.suppressFiles([
      { name: 'photo.jpg', size: 4096, path: 'C:\\moLink\\photo (1).jpg' },
    ]),
    true,
  )
  assert.equal(
    echo.suppressFiles([{ name: 'photo.jpg', size: 4097 }]),
    false,
  )
  assert.equal(
    echo.suppressFiles([
      { name: 'photo.jpg', size: 4096 },
      { name: 'other.jpg', size: 10 },
    ]),
    false,
  )
  assert.equal(echo.suppressFiles([]), false)
})

test('different files are never suppressed', () => {
  const echo = new ClipboardEcho(new MemoryStorage(), clock().now)
  echo.rememberPaths(['C:\\a.txt'])
  assert.equal(echo.suppressPath('C:\\other\\a.txt'), false)
  assert.equal(echo.suppressPath(''), false)
})

test('a malformed storage record is ignored instead of throwing', () => {
  const storage = new MemoryStorage()
  storage.setItem(ECHO_STORAGE_KEY, '{not json')
  const echo = new ClipboardEcho(storage, clock().now)
  assert.equal(echo.suppressPath('C:\\a.txt'), false)
  echo.rememberPaths(['C:\\a.txt'])
  assert.equal(echo.suppressPath('C:\\a.txt'), true)
})

test('offers and paths use distinct keys', () => {
  assert.notEqual(echoOfferKey('a.txt', 3), echoPathKey('a.txt'))
})
