// The one-click diagnostics export: body format, host section when no session
// is live, the 7-day pruned storage, and the recording switch.
import test from 'node:test'
import assert from 'node:assert/strict'

import {
  DIAG_RETENTION_MS,
  __setDiagStorageForTest,
  buildCopyLogText,
  flushDiag,
  isDiagRecordingEnabled,
  logDiag,
  pruneDiagEntries,
  redactDiagnosticText,
  setDiagRecordingEnabled,
} from './diag-log.ts'

function fakeStorage(initial = {}) {
  const items = new Map(Object.entries(initial))
  return {
    getItem(key) {
      return items.has(key) ? items.get(key) : null
    },
    setItem(key, value) {
      items.set(key, String(value))
    },
    removeItem(key) {
      items.delete(key)
    },
    dump() {
      return Object.fromEntries(items)
    },
  }
}

function seedStorage(entries, enabled = true) {
  const storage = fakeStorage()
  if (entries.length > 0) {
    storage.setItem('molink.diag.v1', JSON.stringify(entries))
  }
  storage.setItem('molink.diag.enabled.v1', enabled ? '1' : '0')
  __setDiagStorageForTest(storage)
  return storage
}

function reset() {
  __setDiagStorageForTest(null)
}

test('recording is on by default and the switch persists', () => {
  const storage = fakeStorage()
  assert.equal(isDiagRecordingEnabled(storage), true, 'default is on')
  setDiagRecordingEnabled(false, storage)
  assert.equal(storage.dump()['molink.diag.enabled.v1'], '0')
  assert.equal(isDiagRecordingEnabled(storage), false)
  setDiagRecordingEnabled(true, storage)
  assert.equal(storage.dump()['molink.diag.enabled.v1'], '1')
  assert.equal(isDiagRecordingEnabled(storage), true)
  reset()
})

test('credentials are redacted before storage', () => {
  assert.equal(redactDiagnosticText('token=abc123'), 'token=***')
  assert.equal(
    redactDiagnosticText('session: 0123456789abcdef0123456789abcdef'),
    'session=***',
  )
  assert.equal(redactDiagnosticText('code=ABC-DEF-GHI'), 'code=***')
  assert.equal(redactDiagnosticText('unknown key code: MetaLeft'),
    'unknown key code: MetaLeft')
  assert.equal(redactDiagnosticText('unauthorized'), 'unauthorized')
})

test('entries older than seven days are pruned', () => {
  const now = 1_700_000_000_000
  const fresh = { t: now - 1000, lvl: 'info', ev: 'fresh' }
  const borderline = {
    t: now - (DIAG_RETENTION_MS - 1000),
    lvl: 'info',
    ev: 'borderline',
  }
  const stale = { t: now - DIAG_RETENTION_MS - 1000, lvl: 'info', ev: 'stale' }
  const kept = pruneDiagEntries([stale, fresh, borderline], now)
  assert.deepEqual(
    kept.map((entry) => entry.ev),
    ['fresh', 'borderline'],
  )
})

test('the copied document names both builds and splits local/host', () => {
  const now = Date.now()
  const storage = seedStorage([
    { t: now - 2000, lvl: 'info', ev: 'ws.open', data: { target: 'a:1' } },
    { t: now - 1000, lvl: 'warn', ev: 'frame.inflate', msg: 'broken tile' },
  ])
  logDiag('error', 'session.error', 'boom', { err_code: 'bad_input' })
  flushDiag()
  const text = buildCopyLogText({
    version: '0.12.4',
    hostVersion: '0.12.3',
    connected: true,
    hostTail:
      '2026-11-23T10:00:00.000+08:00 INFO session.welcome peer=192.0.2.1\n',
    localLines: 200,
  })
  const lines = text.split('\n')
  assert.equal(lines[0], '=== moLink diag ===')
  assert.equal(lines[1], 'version: 0.12.4 / host 0.12.3')
  assert.match(lines[2], /^time: \d{4}-\d\d-\d\dT/)
  assert.equal(lines[3], '--- local ---')
  const hostHeader = lines.indexOf('--- host ---')
  assert.ok(hostHeader > 3)
  const hostBody = lines.slice(hostHeader + 1).join('\n')
  assert.match(hostBody, /session\.welcome/)
  const localBody = lines.slice(4, hostHeader).join('\n')
  assert.match(localBody, /ws\.open/)
  assert.match(localBody, /broken tile/)
  assert.ok(storage.dump()['molink.diag.v1'].includes('session.error'))
  reset()
})

test('the host section says "not connected" without a session', () => {
  seedStorage([])
  const text = buildCopyLogText({
    version: '0.12.4',
    connected: false,
    hostTail: 'must not appear',
  })
  const lines = text.split('\n')
  assert.equal(lines[1], 'version: 0.12.4 / host unknown')
  assert.equal(lines[lines.length - 2], 'not connected')
  assert.ok(!text.includes('must not appear'))
  reset()
})

test('switching recording off stops storage writes but keeps the ring', () => {
  const storage = seedStorage([], true)
  setDiagRecordingEnabled(false, storage)
  logDiag('info', 'quiet.event', 'still in memory')
  flushDiag()
  assert.equal(storage.getItem('molink.diag.v1'), null, 'no file while off')
  const text = buildCopyLogText({ version: 'x', connected: false })
  assert.match(text, /quiet\.event/, 'the in-memory entry still exports')

  setDiagRecordingEnabled(true, storage)
  flushDiag()
  assert.match(
    storage.getItem('molink.diag.v1') ?? '',
    /quiet\.event/,
    'turning it on writes the buffer',
  )
  reset()
})
