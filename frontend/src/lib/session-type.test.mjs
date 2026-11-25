// The Wayland-only-XWayland notice: it appears for a Wayland host once, and a
// dismissal persists so it never comes back.
import test from 'node:test'
import assert from 'node:assert/strict'

import {
  WAYLAND_NOTICE_DISMISS_KEY,
  dismissWaylandNotice,
  normalizeSessionType,
  shouldShowWaylandNotice,
  waylandNoticeDismissed,
} from './session-type.ts'

function fakeStorage(initial = {}) {
  const items = new Map(Object.entries(initial))
  return {
    getItem(key) {
      return items.has(key) ? items.get(key) : null
    },
    setItem(key, value) {
      items.set(key, String(value))
    },
    dump() {
      return Object.fromEntries(items)
    },
  }
}

test('only x11 and wayland are real session types', () => {
  assert.equal(normalizeSessionType('wayland'), 'wayland')
  assert.equal(normalizeSessionType('x11'), 'x11')
  assert.equal(normalizeSessionType(undefined), 'unknown')
  assert.equal(normalizeSessionType(null), 'unknown')
  assert.equal(normalizeSessionType(''), 'unknown')
  assert.equal(normalizeSessionType('WAYLAND'), 'unknown')
})

test('the notice appears for a Wayland host and only then', () => {
  assert.equal(shouldShowWaylandNotice('wayland', false), true)
  assert.equal(shouldShowWaylandNotice('x11', false), false)
  assert.equal(shouldShowWaylandNotice('unknown', false), false)
  assert.equal(shouldShowWaylandNotice(undefined, false), false)
  // Blocked storage reads as "not dismissed" and keeps the warning visible.
  assert.equal(shouldShowWaylandNotice('wayland', false), true)
})

test('a dismissal persists and hides the notice from then on', () => {
  const storage = fakeStorage()
  assert.equal(waylandNoticeDismissed(storage), false)
  assert.equal(
    shouldShowWaylandNotice('wayland', waylandNoticeDismissed(storage)),
    true,
  )

  dismissWaylandNotice(storage)
  assert.equal(storage.dump()[WAYLAND_NOTICE_DISMISS_KEY], '1')
  assert.equal(waylandNoticeDismissed(storage), true)
  assert.equal(
    shouldShowWaylandNotice('wayland', waylandNoticeDismissed(storage)),
    false,
  )

  // A fresh read of the same storage (next launch) stays dismissed.
  assert.equal(
    shouldShowWaylandNotice('wayland', waylandNoticeDismissed(storage)),
    false,
  )
})

test('storage failures never break the UI', () => {
  const broken = {
    getItem() {
      throw new Error('blocked')
    },
    setItem() {
      throw new Error('blocked')
    },
  }
  assert.equal(waylandNoticeDismissed(broken), false)
  assert.doesNotThrow(() => dismissWaylandNotice(broken))
  assert.equal(waylandNoticeDismissed(null), false)
  assert.doesNotThrow(() => dismissWaylandNotice(null))
})
