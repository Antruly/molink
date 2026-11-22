// Unit tests for the persistent reconnect-credential store.
//
// Run with: npm test  (Node's built-in runner strips the TypeScript types).
import test from 'node:test'
import assert from 'node:assert/strict'

import {
  SessionStore,
  kSessionKeyPrefix,
  kSessionLifetimeMs,
  sessionStorageKey,
} from './session-store.ts'

/// A localStorage stand-in that preserves insertion order like the real one.
function fakeStorage(initial = {}) {
  const items = new Map(Object.entries(initial))
  return {
    get length() {
      return items.size
    },
    key(index) {
      return [...items.keys()][index] ?? null
    },
    getItem(key) {
      return items.has(key) ? items.get(key) : null
    },
    setItem(key, value) {
      items.set(key, String(value))
    },
    removeItem(key) {
      items.delete(key)
    },
    /// Test helper: raw view of the backing map.
    dump() {
      return Object.fromEntries(items)
    },
  }
}

function storeAt(storage, now = () => 1000) {
  return new SessionStore({ storage, now })
}

test('a welcome token is written under molink.session.<host>:<port>', () => {
  const storage = fakeStorage()
  const store = storeAt(storage)
  store.rememberToken('192.168.2.20', 28418, 'tok-a')

  assert.equal(store.tokenFor('192.168.2.20', 28418), 'tok-a')
  assert.deepEqual(storage.dump(), {
    [kSessionKeyPrefix + '192.168.2.20:28418']: JSON.stringify({
      token: 'tok-a',
      savedAt: 1000,
    }),
  })
  assert.equal(
    sessionStorageKey('192.168.2.20', 28418),
    'molink.session.192.168.2.20:28418',
  )
})

test('credentials expire seven days after they were received', () => {
  let now = 10_000
  const storage = fakeStorage()
  const store = storeAt(storage, () => now)
  store.rememberToken('host', 1, 'tok')

  now += kSessionLifetimeMs
  assert.equal(store.tokenFor('host', 1), 'tok', 'the boundary is still valid')

  now += 1
  assert.equal(store.tokenFor('host', 1), '', 'one millisecond later is stale')
  assert.deepEqual(storage.dump(), {}, 'the expired key is removed from disk')
})

test('an expired credential loaded from disk is discarded', () => {
  const savedAt = 1000
  const storage = fakeStorage({
    'molink.session.host:1': JSON.stringify({ token: 'old', savedAt }),
  })
  const store = storeAt(storage, () => savedAt + kSessionLifetimeMs + 1)
  assert.equal(store.tokenFor('host', 1), '')
})

test('forgetToken removes a credential the host rejected', () => {
  const storage = fakeStorage()
  const store = storeAt(storage)
  store.rememberToken('host', 1, 'bad')
  store.forgetToken('host', 1)

  assert.equal(store.tokenFor('host', 1), '')
  assert.deepEqual(storage.dump(), {})
})

test('remember_device=false keeps credentials in memory only', () => {
  const storage = fakeStorage()
  const store = storeAt(storage)
  store.setRememberDevice(false)
  store.rememberToken('host', 1, 'live')

  assert.equal(store.tokenFor('host', 1), 'live', 'the open tab can reconnect')
  assert.deepEqual(storage.dump(), {}, 'nothing reaches localStorage')
  assert.deepEqual(store.persistedRecords(), {})
})

test('turning remember_device off clears credentials already on disk', () => {
  const storage = fakeStorage()
  const store = storeAt(storage)
  store.rememberToken('host', 1, 'kept')
  assert.equal(Object.keys(storage.dump()).length, 1)

  store.setRememberDevice(false)
  assert.deepEqual(storage.dump(), {})
  assert.equal(store.tokenFor('host', 1), 'kept', 'the in-memory copy stays')
})

test('turning remember_device back on persists the current session', () => {
  const storage = fakeStorage()
  const store = storeAt(storage)
  store.setRememberDevice(false)
  store.rememberToken('host', 1, 'live')
  assert.deepEqual(storage.dump(), {})

  store.setRememberDevice(true)
  assert.equal(
    JSON.parse(storage.getItem('molink.session.host:1')).token,
    'live',
  )
})

test('the same host on another port never reuses a credential', () => {
  const storage = fakeStorage()
  const store = storeAt(storage)
  store.rememberToken('192.168.2.20', 28418, 'port-a')

  assert.equal(store.tokenFor('192.168.2.20', 28418), 'port-a')
  assert.equal(store.tokenFor('192.168.2.20', 28419), '')
})

test('different hosts never reuse a credential', () => {
  const store = storeAt(fakeStorage())
  store.rememberToken('192.168.2.20', 28418, 'host-a')
  assert.equal(store.tokenFor('192.168.2.21', 28418), '')
})

test('an external (ui.json) backend round-trips through hydrate/save', () => {
  const saved = []
  const store = new SessionStore({ storage: null, now: () => 5000 })
  store.useExternalPersistence((records) => saved.push(records))

  assert.equal(store.backendKind, 'external')
  store.rememberToken('host', 1, 'file-token')
  assert.deepEqual(saved.at(-1), {
    'molink.session.host:1': { token: 'file-token', savedAt: 5000 },
  })

  // A fresh store reads the same document the file backend wrote.
  const reloaded = new SessionStore({ storage: null, now: () => 6000 })
  reloaded.useExternalPersistence(() => undefined)
  reloaded.hydrate(saved.at(-1))
  assert.equal(reloaded.tokenFor('host', 1), 'file-token')
})

test('hydrate accepts a bare host:port key and drops malformed entries', () => {
  const store = new SessionStore({ storage: null, now: () => 1000 })
  store.useExternalPersistence(() => undefined)
  store.hydrate({
    'host:1': { token: 'bare', savedAt: 900 },
    'molink.session.host:2': { token: '', savedAt: 900 },
    'molink.session.host:3': 'not-an-object',
    'molink.session.host:4': {
      token: 'stale',
      savedAt: 1000 - kSessionLifetimeMs - 1,
    },
  })

  assert.equal(store.tokenFor('host', 1), 'bare')
  assert.equal(store.tokenFor('host', 2), '')
  assert.equal(store.tokenFor('host', 3), '')
  assert.equal(store.tokenFor('host', 4), '')
})

test('hydrate keeps a fresher in-memory token over an older document', () => {
  let now = 9000
  const store = new SessionStore({ storage: null, now: () => now })
  store.useExternalPersistence(() => undefined)
  store.rememberToken('host', 1, 'fresh')

  // The ui.json read was started before the welcome; its copy is older.
  store.hydrate({ 'molink.session.host:1': { token: 'old', savedAt: 1000 } })
  assert.equal(store.tokenFor('host', 1), 'fresh')
})

test('a corrupt localStorage entry is removed instead of crashing', () => {
  const storage = fakeStorage({ 'molink.session.host:1': '{not json' })
  const store = storeAt(storage)
  assert.equal(store.tokenFor('host', 1), '')
  assert.deepEqual(storage.dump(), {})
})
