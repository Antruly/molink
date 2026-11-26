// The simple home offers four session modes; the connect card and the recent
// device shortcut row both render this list.
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test from 'node:test'

import { SESSION_MODES } from '../src/lib/session-modes.ts'

const zh = JSON.parse(
  readFileSync(new URL('../src/locales/zh-CN.json', import.meta.url)),
)
const en = JSON.parse(
  readFileSync(new URL('../src/locales/en.json', import.meta.url)),
)

function translate(locale, key) {
  return key.split('.').reduce((value, part) => value?.[part], locale)
}

test('desktop, files, chat and terminal are the four session modes', () => {
  assert.deepEqual(
    SESSION_MODES.map((mode) => mode.intent),
    ['desktop', 'files', 'chat', 'terminal'],
  )
  // The shortcut row and the connect card must offer one button each.
  assert.equal(SESSION_MODES.length, 4)
})

test('every mode has a label in both locales', () => {
  for (const mode of SESSION_MODES) {
    const zhLabel = translate(zh, mode.labelKey)
    const enLabel = translate(en, mode.labelKey)
    assert.ok(zhLabel, `missing zh-CN ${mode.labelKey}`)
    assert.ok(enLabel, `missing en ${mode.labelKey}`)
  }
  assert.equal(translate(zh, 'simple.modeTerminal'), '远程终端')
  assert.equal(translate(en, 'simple.modeTerminal'), 'Remote terminal')
})
