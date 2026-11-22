// Alias table and fallback rules for browser key events.
//
// Run with: npm test  (Node's built-in test runner strips the TypeScript
// types of the imported module without a build step).
import test from 'node:test'
import assert from 'node:assert/strict'

import { keyToCode, normalizeKeyCode } from './key-code.ts'

test('the Windows key aliases all become MetaLeft/MetaRight', () => {
  for (const alias of ['OSLeft', 'Win', 'Super', '超', 'Meta', 'OS']) {
    assert.equal(normalizeKeyCode(alias), 'MetaLeft', alias)
  }
  assert.equal(normalizeKeyCode('OSRight'), 'MetaRight')
})

test('lowercase letters and digits become physical codes', () => {
  assert.equal(normalizeKeyCode('r'), 'KeyR')
  assert.equal(normalizeKeyCode('7'), 'Digit7')
  // keyToCode is the event.key fallback and follows the same rule.
  assert.equal(keyToCode('r'), 'KeyR')
  assert.equal(keyToCode('R'), 'KeyR')
  assert.equal(keyToCode('7'), 'Digit7')
})

test('named keys and punctuation keep their canonical names', () => {
  assert.equal(keyToCode('Enter'), 'Enter')
  assert.equal(keyToCode('ArrowLeft'), 'ArrowLeft')
  assert.equal(keyToCode(' '), 'Space')
  assert.equal(keyToCode('/'), 'Slash')
  assert.equal(normalizeKeyCode('MetaLeft'), 'MetaLeft')
})

test('unknown input returns empty instead of guessing', () => {
  assert.equal(keyToCode('Unidentified'), '')
  assert.equal(keyToCode('Launch5'), '')
  assert.equal(keyToCode(''), '')
  assert.equal(normalizeKeyCode('Unidentified'), '')
})

test('a whole combo keeps every physical code', () => {
  const combo = ['MetaLeft', 'KeyR'].map(normalizeKeyCode)
  assert.deepEqual(combo, ['MetaLeft', 'KeyR'])
})
