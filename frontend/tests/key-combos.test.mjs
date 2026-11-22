import assert from 'node:assert/strict'
import test from 'node:test'
import {
  KEY_COMBOS,
  findKeyCombo,
  keySequence,
} from '../src/lib/key-combos.ts'

const expectedKeys = new Map([
  ['win', ['MetaLeft']],
  ['win-r', ['MetaLeft', 'KeyR']],
  ['win-e', ['MetaLeft', 'KeyE']],
  ['win-d', ['MetaLeft', 'KeyD']],
  ['win-tab', ['MetaLeft', 'Tab']],
  ['win-l', ['MetaLeft', 'KeyL']],
  ['alt-tab', ['AltLeft', 'Tab']],
  ['alt-f4', ['AltLeft', 'F4']],
  ['ctrl-esc', ['ControlLeft', 'Escape']],
  ['ctrl-shift-esc', ['ControlLeft', 'ShiftLeft', 'Escape']],
  ['win-shift-s', ['MetaLeft', 'ShiftLeft', 'KeyS']],
  ['print-screen', ['PrintScreen']],
])

test('the menu covers every requested shortcut', () => {
  assert.deepEqual(
    KEY_COMBOS.map((combo) => combo.id),
    [...expectedKeys.keys()],
  )
  for (const combo of KEY_COMBOS) {
    assert.deepEqual([...combo.keys], expectedKeys.get(combo.id), combo.id)
    assert.ok(combo.display.length > 0, combo.id)
    assert.match(combo.labelKey, /^session\.combo\./, combo.id)
  }
})

test('Win+R presses the modifier first, then presses and releases R', () => {
  assert.deepEqual(keySequence(findKeyCombo('win-r').keys), [
    { code: 'MetaLeft', down: true },
    { code: 'KeyR', down: true },
    { code: 'KeyR', down: false },
    { code: 'MetaLeft', down: false },
  ])
})

test('multi-modifier shortcuts release in reverse press order', () => {
  assert.deepEqual(keySequence(findKeyCombo('win-shift-s').keys), [
    { code: 'MetaLeft', down: true },
    { code: 'ShiftLeft', down: true },
    { code: 'KeyS', down: true },
    { code: 'KeyS', down: false },
    { code: 'ShiftLeft', down: false },
    { code: 'MetaLeft', down: false },
  ])
})

test('Ctrl+Shift+Esc and PrintScreen stay balanced', () => {
  assert.deepEqual(keySequence(findKeyCombo('ctrl-shift-esc').keys), [
    { code: 'ControlLeft', down: true },
    { code: 'ShiftLeft', down: true },
    { code: 'Escape', down: true },
    { code: 'Escape', down: false },
    { code: 'ShiftLeft', down: false },
    { code: 'ControlLeft', down: false },
  ])
  assert.deepEqual(keySequence(findKeyCombo('print-screen').keys), [
    { code: 'PrintScreen', down: true },
    { code: 'PrintScreen', down: false },
  ])
})

test('every key that goes down is released exactly once', () => {
  for (const combo of KEY_COMBOS) {
    assert.equal(
      keySequence(combo.keys).length,
      combo.keys.length * 2,
      combo.id,
    )
    const downs = keySequence(combo.keys).filter((step) => step.down)
    const ups = keySequence(combo.keys).filter((step) => !step.down)
    assert.deepEqual(
      downs.map((step) => step.code),
      [...combo.keys],
      combo.id,
    )
    assert.deepEqual(
      ups.map((step) => step.code),
      [...combo.keys].reverse(),
      combo.id,
    )
  }
})

test('only Win+L asks for confirmation before locking the host', () => {
  for (const combo of KEY_COMBOS) {
    assert.equal(combo.confirmKey !== undefined, combo.id === 'win-l', combo.id)
  }
})
