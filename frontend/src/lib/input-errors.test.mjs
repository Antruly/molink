// Routing rules for host error frames: input problems stay out of the session
// banner, credentials are removed before diagnostics.
import test from 'node:test'
import assert from 'node:assert/strict'

import { isInputErrorCode, redactDiagnosticText } from './input-errors.ts'

test('input rejections never become session errors', () => {
  for (const code of [
    'bad_input',
    'input_unavailable',
    'unknown_input',
    'unknown_key',
    'bad_key',
    'input_rejected',
  ]) {
    assert.equal(isInputErrorCode(code), true, code)
  }
})

test('connection and policy errors stay session errors', () => {
  for (const code of [
    'unauthorized',
    'ip_banned',
    'too_many_attempts',
    'display_mode_failed',
    'bad_codec',
    'send_failed',
    '',
  ]) {
    assert.equal(isInputErrorCode(code), false, code)
  }
})

test('the failing key survives, credentials do not', () => {
  assert.equal(
    redactDiagnosticText('unknown key code: MetaLeft'),
    'unknown key code: MetaLeft',
  )
  const redacted = redactDiagnosticText(
    'token=abc123 Bearer eyJhbGciOi.xyz ?code=9999',
  )
  assert.ok(!redacted.includes('abc123'), redacted)
  assert.ok(!redacted.includes('eyJhbGciOi'), redacted)
  assert.ok(!redacted.includes('9999'), redacted)
})
