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

test('the keypad fallback keeps Numpad codes', () => {
  // DOM_KEY_LOCATION_NUMPAD === 3. The keypad reports the same printable
  // event.key as the main row, so location is what tells them apart.
  assert.equal(keyToCode('0', 3), 'Numpad0')
  assert.equal(keyToCode('1', 3), 'Numpad1')
  assert.equal(keyToCode('9', 3), 'Numpad9')
  assert.equal(keyToCode('+', 3), 'NumpadAdd')
  assert.equal(keyToCode('-', 3), 'NumpadSubtract')
  assert.equal(keyToCode('*', 3), 'NumpadMultiply')
  assert.equal(keyToCode('/', 3), 'NumpadDivide')
  assert.equal(keyToCode('.', 3), 'NumpadDecimal')
  assert.equal(keyToCode(',', 3), 'NumpadComma')
  assert.equal(keyToCode('=', 3), 'NumpadEqual')
  assert.equal(keyToCode('Enter', 3), 'NumpadEnter')
  // A keypad Clear (NumLock off) stays Clear, not Numpad5.
  assert.equal(keyToCode('Clear', 3), 'Clear')
  // Without the keypad location the same key is the main row.
  assert.equal(keyToCode('1'), 'Digit1')
  assert.equal(keyToCode('-'), 'Minus')
  assert.equal(keyToCode('Enter'), 'Enter')
})

test('function, media, browser and IME names survive the fallback', () => {
  for (const name of [
    'F13',
    'F24',
    'Insert',
    'PrintScreen',
    'AudioVolumeUp',
    'MediaPlayPause',
    'BrowserBack',
    'LaunchApp1',
    'Again',
    'Power',
    'Lang1',
    'HangulMode',
  ]) {
    assert.equal(keyToCode(name), name, name)
  }
  // F25 is outside the DOM set.
  assert.equal(keyToCode('F25'), '')
})
