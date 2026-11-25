import assert from 'node:assert/strict'
import test from 'node:test'
import {
  HOST_ONLY_FALLBACK_PORT,
  HOST_ONLY_LOCAL_HOST,
  homeLayout,
} from '../src/lib/host-only.ts'

test('a normal install keeps the connect form and the device list', () => {
  for (const value of [false, undefined]) {
    const layout = homeLayout(value, 28418)
    assert.equal(layout.hostOnly, false)
    assert.equal(layout.showConnectForm, true)
    assert.equal(layout.showDeviceList, true)
    assert.equal(layout.localScreen, null)
  }
})

test('host-only replaces every outbound target with the local screen', () => {
  const layout = homeLayout(true, 29000)
  assert.equal(layout.hostOnly, true)
  assert.equal(layout.showConnectForm, false)
  assert.equal(layout.showDeviceList, false)
  assert.deepEqual(layout.localScreen, {
    host: HOST_ONLY_LOCAL_HOST,
    port: 29000,
  })
  assert.equal(layout.localScreen.host, '127.0.0.1')
})

test('host-only falls back when the state has no usable port', () => {
  for (const port of [undefined, 0, -1, Number.NaN, 70000]) {
    assert.equal(
      homeLayout(true, port).localScreen.port,
      HOST_ONLY_FALLBACK_PORT,
    )
  }
})
