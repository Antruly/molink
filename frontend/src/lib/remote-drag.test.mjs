// The drag-out payload and the "pointer left the window" rules. The shell is
// the only consumer of the payload, so its shape is part of the bridge.
import test from 'node:test'
import assert from 'node:assert/strict'

import {
  buildRemoteDragPayload,
  dragEndedOutside,
  dragItemsFor,
  encodeRemoteDragPayload,
  isOutsideWindow,
  parseRemoteDragPayload,
} from './remote-drag.ts'

test('the payload carries one path and one name per item', () => {
  const payload = buildRemoteDragPayload([
    { path: '/home/me/a.txt', name: 'a.txt', dir: false },
    { path: '/home/me/photos', name: 'photos', dir: true },
  ])
  assert.deepEqual(payload, {
    paths: ['/home/me/a.txt', '/home/me/photos'],
    names: ['a.txt', 'photos'],
  })
})

test('encode/parse round-trips and rejects junk', () => {
  const raw = encodeRemoteDragPayload({
    paths: ['/a/b c.txt', '/a/dir'],
    names: ['b c.txt', 'dir'],
  })
  assert.deepEqual(parseRemoteDragPayload(raw), {
    paths: ['/a/b c.txt', '/a/dir'],
    names: ['b c.txt', 'dir'],
  })
  assert.equal(parseRemoteDragPayload(null), null)
  assert.equal(parseRemoteDragPayload('not json'), null)
  assert.equal(parseRemoteDragPayload('{"paths":[],"names":[]}'), null)
  assert.equal(parseRemoteDragPayload('{"paths":["/a"],"names":"x"}'), null)
})

test('an empty name borrows the last path segment', () => {
  assert.deepEqual(
    parseRemoteDragPayload('{"paths":["/a/photo.jpg"],"names":[""]}'),
    { paths: ['/a/photo.jpg'], names: ['photo.jpg'] },
  )
})

test('a selected row drags the whole selection; a plain row drags itself', () => {
  const selected = [
    { path: '/a/one', name: 'one', dir: false },
    { path: '/a/two', name: 'two', dir: false },
  ]
  assert.deepEqual(
    dragItemsFor({ path: '/a/one', name: 'one', dir: false }, selected),
    selected,
  )
  assert.deepEqual(
    dragItemsFor({ path: '/a/three', name: 'three', dir: false }, selected),
    [{ path: '/a/three', name: 'three', dir: false }],
  )
})

test('the window edge and a cancelled drop mean the desktop', () => {
  assert.equal(isOutsideWindow(100, 100, 800, 600), false)
  assert.equal(isOutsideWindow(0, 100, 800, 600), true)
  assert.equal(isOutsideWindow(100, 600, 800, 600), true)
  assert.equal(isOutsideWindow(-4, 100, 800, 600), true)
  // A drop the page accepted stays a normal in-page drop.
  assert.equal(dragEndedOutside('copy', true, 0, 0, 800, 600), false)
  assert.equal(dragEndedOutside('none', false, 400, 300, 800, 600), false)
  assert.equal(dragEndedOutside('none', true, 400, 300, 800, 600), true)
  assert.equal(dragEndedOutside('none', false, 799, 300, 800, 600), true)
})
