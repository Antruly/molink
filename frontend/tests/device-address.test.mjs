import assert from 'node:assert/strict'
import test from 'node:test'
import { deviceEndpoints, primaryHost } from '../src/lib/device-address.ts'

test('every address becomes its own ip:port, in the core order', () => {
  assert.deepEqual(
    deviceEndpoints(
      ['10.0.0.8', '172.17.0.1', '192.168.1.20'],
      28418,
      'fallback.local',
    ),
    ['10.0.0.8:28418', '172.17.0.1:28418', '192.168.1.20:28418'],
  )
})

test('the core private-first order is preserved, not re-sorted', () => {
  const addresses = ['192.168.1.20', '10.1.2.3', '172.20.0.1']
  assert.deepEqual(
    deviceEndpoints(addresses, 28418, 'host').map((value) =>
      value.split(':')[0],
    ),
    addresses,
  )
})

test('an empty address list falls back to the page hostname', () => {
  assert.deepEqual(deviceEndpoints([], 28418, 'molink.local'), [
    'molink.local:28418',
  ])
  assert.deepEqual(
    deviceEndpoints(undefined, '28418', 'molink.local'),
    ['molink.local:28418'],
  )
})

test('blank entries are ignored and a missing port leaves the host alone', () => {
  assert.deepEqual(deviceEndpoints(['', '  ', '192.168.1.5'], null, 'host'), [
    '192.168.1.5',
  ])
  assert.deepEqual(deviceEndpoints(['192.168.1.5'], undefined, 'host'), [
    '192.168.1.5',
  ])
})

test('the primary host is the first address, with no port', () => {
  assert.equal(
    primaryHost(['192.168.1.20', '10.0.0.8'], 'fallback.local'),
    '192.168.1.20',
  )
  assert.equal(primaryHost([], 'fallback.local'), 'fallback.local')
})
