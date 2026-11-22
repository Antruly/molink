import assert from 'node:assert/strict'
import test from 'node:test'
import {
  serviceActionFailure,
  servicePanelHints,
  serviceRecoveryHint,
} from '../src/lib/service-panel.ts'

const windowsInstalled = {
  available: true,
  installed: true,
  autostart: false,
  stale: false,
  elevated: false,
}

test('an installed Windows service still offers re-registration and UAC', () => {
  assert.deepEqual(servicePanelHints(windowsInstalled), {
    windows: true,
    showReregister: true,
    reregisterLabel: 'reregister',
    showUacHint: true,
    showStaleWarning: false,
  })
})

test('an elevated core hides the UAC hint', () => {
  const hints = servicePanelHints({ ...windowsInstalled, elevated: true })
  assert.equal(hints.showReregister, true)
  assert.equal(hints.showUacHint, false)
})

test('a stale registration shows the warning', () => {
  assert.equal(
    servicePanelHints({ ...windowsInstalled, stale: true }).showStaleWarning,
    true,
  )
})

test('a missing Windows service is labelled install, not re-register', () => {
  const hints = servicePanelHints({
    ...windowsInstalled,
    installed: false,
  })
  assert.equal(hints.showReregister, true)
  assert.equal(hints.reregisterLabel, 'install')
})

test('the Linux login autostart has no Windows re-registration or UAC text', () => {
  assert.deepEqual(
    servicePanelHints({
      available: true,
      installed: true,
      autostart: true,
      stale: true,
      elevated: true,
    }),
    {
      windows: false,
      showReregister: false,
      reregisterLabel: 'reregister',
      showUacHint: false,
      showStaleWarning: false,
    },
  )
})

test('a missing helper points at the portable install, other errors at a manual run', () => {
  assert.equal(
    serviceRecoveryHint('molink-service.exe is not installed next to the core'),
    'portable',
  )
  assert.equal(
    serviceRecoveryHint('could not start the elevated service helper'),
    'manual',
  )
  assert.equal(serviceRecoveryHint(null), null)
  assert.equal(serviceRecoveryHint(''), null)
})

test('the backend error text is kept verbatim', () => {
  const caught = new Error('OpenSCManager failed')
  assert.deepEqual(serviceActionFailure(caught, 'fallback'), {
    message: 'OpenSCManager failed',
    recovery: 'manual',
  })
  assert.deepEqual(serviceActionFailure('not an Error', 'fallback'), {
    message: 'fallback',
    recovery: 'manual',
  })
})
