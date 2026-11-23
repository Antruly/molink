// Failure routing: a clipboard/input/file problem is a toast and must leave
// the session banner alone; a link failure is a banner that may offer
// reconnect.
import test from 'node:test'
import assert from 'node:assert/strict'

import {
  TOAST_TTL_MS,
  applyFailure,
  classifyError,
  needsReconnect,
  shouldClearToast,
  toastFor,
} from './session-alerts.ts'

const idle = { error: null, errorCode: null, toast: null }

test('a remote clipboard failure becomes a toast, not a session error', () => {
  const result = applyFailure(
    idle,
    'remote_clipboard_failed',
    'remote_clipboard_failed',
    1,
    1000,
  )
  assert.equal(result.surface, 'toast')
  assert.equal(result.error, null)
  assert.equal(result.errorCode, null)
  assert.equal(result.reconnect, false)
  assert.notEqual(result.toast, null)
  assert.equal(result.toast.message, 'remote_clipboard_failed')
  assert.equal(result.toast.tone, 'error')
})

test('a clipboard failure keeps an existing banner untouched', () => {
  const state = {
    error: 'display_mode_failed',
    errorCode: 'display_mode_failed',
    toast: null,
  }
  const result = applyFailure(
    state,
    'clipboard_files_failed',
    'clipboard_files_failed',
    1,
    1000,
  )
  assert.equal(result.error, 'display_mode_failed')
  assert.equal(result.errorCode, 'display_mode_failed')
  assert.equal(result.toast.message, 'clipboard_files_failed')
})

test('a toast clears six seconds after its last refresh', () => {
  const toast = toastFor(null, 'remote_clipboard_too_large', 'warn', 7, 1000)
  assert.equal(TOAST_TTL_MS, 6000)
  assert.equal(shouldClearToast(toast, 7, 6999), false)
  assert.equal(shouldClearToast(toast, 7, 7000), true)
  // A newer toast with a different id must not be cleared by the old timer.
  assert.equal(shouldClearToast({ ...toast, id: 8 }, 7, 7000), false)
})

test('repeating the same failure keeps one toast and refreshes its timer', () => {
  const first = applyFailure(
    idle,
    'remote_clipboard_failed',
    'remote_clipboard_failed',
    1,
    1000,
  )
  const second = applyFailure(
    first,
    'remote_clipboard_failed',
    'remote_clipboard_failed',
    2,
    3000,
  )
  assert.equal(second.toast.id, first.toast.id)
  assert.equal(second.toast.at, 3000)
  assert.equal(
    shouldClearToast(second.toast, second.toast.id, 8999),
    false,
  )
  assert.equal(
    shouldClearToast(second.toast, second.toast.id, 9000),
    true,
  )
})

test('a transport close stays a session error that offers reconnect', () => {
  const result = applyFailure(
    idle,
    'connection lost',
    'connection_lost',
    2,
    0,
  )
  assert.equal(result.surface, 'banner')
  assert.equal(result.error, 'connection lost')
  assert.equal(result.reconnect, true)
  assert.equal(result.toast, null)
})

test('credential, handshake and transport failures offer reconnect', () => {
  for (const code of [
    'unauthorized',
    'invalid_token',
    'too_many_attempts',
    'ip_banned',
    'hello_timeout',
    'handshake_failed',
    'connection_lost',
    'transport_closed',
  ]) {
    assert.equal(needsReconnect(code, ''), true, code)
  }
  for (const code of [
    'display_mode_failed',
    'bad_preset',
    'bad_codec',
    'remote_clipboard_failed',
    '',
  ]) {
    assert.equal(needsReconnect(code, code), false, code)
  }
})

test('transport closes are recognised even without a machine code', () => {
  const result = applyFailure(idle, 'closed (1008): access denied', null, 3, 0)
  assert.equal(result.surface, 'banner')
  assert.equal(result.reconnect, true)
  assert.equal(needsReconnect(null, 'the relay is unavailable'), true)
  assert.equal(needsReconnect(null, 'invalid endpoint'), false)
})

test('input and file failures never enter the session banner', () => {
  for (const code of [
    'bad_input',
    'unknown_key',
    'input_unavailable',
    'bad_file_action',
    'file_error',
  ]) {
    const route = classifyError(code, code)
    assert.equal(route.surface, 'toast', code)
    assert.equal(route.reconnect, false, code)
  }
})

test('informational clipboard notes use the info tone', () => {
  assert.equal(
    classifyError('clipboard_files_staged', 'clipboard_files_staged').tone,
    'info',
  )
  assert.equal(
    classifyError('remote_clipboard_too_large', 'remote_clipboard_too_large')
      .tone,
    'warn',
  )
})

test('host prose is shown as sent; a bare code stays translatable', () => {
  assert.equal(
    applyFailure(idle, 'clipboard text exceeds 192 KiB', 'clipboard_too_large', 1, 0)
      .toast.message,
    'clipboard text exceeds 192 KiB',
  )
  assert.equal(
    applyFailure(idle, 'clipboard_files_failed', 'clipboard_files_failed', 1, 0)
      .toast.message,
    'clipboard_files_failed',
  )
})
