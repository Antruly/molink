// The session's chat/files/terminal entries are shared by two rendered
// paths: the wide toolbar (icon buttons) and the compact tools menu. These
// tests render both through react-dom so a change that drops the terminal
// from one path fails here, not just on a narrow phone-width window.
import assert from 'node:assert/strict'
import test from 'node:test'
import { createElement } from 'react'
import { renderToStaticMarkup } from 'react-dom/server'

import {
  sessionToolEntries,
  terminalEntryVisible,
} from '../src/lib/session-tools.ts'

const labels = {
  chatLabel: '聊天',
  filesLabel: '文件',
  terminalLabel: '终端',
}

function entries(phase, terminalDisabled) {
  return sessionToolEntries({ phase, terminalDisabled, ...labels })
}

/// Mirrors the wide toolbar: each entry is an icon button whose visible name
/// is its aria-label/title.
function WideToolbar({ phase, terminalDisabled }) {
  return createElement(
    'div',
    { 'data-bar': 'wide' },
    entries(phase, terminalDisabled).map((entry) =>
      createElement('button', {
        key: entry.key,
        type: 'button',
        title: entry.label,
        'aria-label': entry.label,
        'data-entry': entry.key,
      }),
    ),
  )
}

/// Mirrors the compact tools menu: each entry is a menuitem with its label
/// as text.
function CompactToolsMenu({ phase, terminalDisabled }) {
  return createElement(
    'div',
    { role: 'menu', 'data-bar': 'compact' },
    entries(phase, terminalDisabled).map((entry) =>
      createElement(
        'button',
        {
          key: entry.key,
          type: 'button',
          role: 'menuitem',
          'data-entry': entry.key,
        },
        entry.label,
      ),
    ),
  )
}

const render = (component, phase, terminalDisabled) =>
  renderToStaticMarkup(createElement(component, { phase, terminalDisabled }))

test('terminal is available exactly on a live, non-refused session', () => {
  assert.equal(terminalEntryVisible('live', false), true)
  assert.equal(terminalEntryVisible('live', true), false)
  for (const phase of ['idle', 'connecting', 'error', 'disconnected']) {
    assert.equal(terminalEntryVisible(phase, false), false, phase)
  }
})

test('chat, files and terminal are listed on a live session', () => {
  assert.deepEqual(
    entries('live', false).map((entry) => entry.key),
    ['chat', 'files', 'terminal'],
  )
  // Chat and files stay available before the welcome; only the terminal
  // waits for a live session.
  assert.deepEqual(
    entries('connecting', false).map((entry) => entry.key),
    ['chat', 'files'],
  )
})

test('a live session renders the terminal entry in the wide toolbar and the compact menu', () => {
  const wide = render(WideToolbar, 'live', false)
  const compact = render(CompactToolsMenu, 'live', false)

  assert.match(wide, /data-entry="terminal"/)
  assert.match(wide, /aria-label="终端"/)
  assert.match(compact, /data-entry="terminal"/)
  assert.match(compact, />终端</)
})

test('a refused terminal renders in neither the wide toolbar nor the compact menu', () => {
  const wide = render(WideToolbar, 'live', true)
  const compact = render(CompactToolsMenu, 'live', true)

  for (const html of [wide, compact]) {
    assert.doesNotMatch(html, /data-entry="terminal"/)
    assert.doesNotMatch(html, /终端/)
    // The other session entries are never collateral damage.
    assert.match(html, /data-entry="chat"/)
    assert.match(html, /data-entry="files"/)
  }
})

test('a session that is not live renders no terminal entry on either path', () => {
  for (const phase of ['connecting', 'disconnected', 'error']) {
    const wide = render(WideToolbar, phase, false)
    const compact = render(CompactToolsMenu, phase, false)
    assert.doesNotMatch(wide, /data-entry="terminal"/, phase)
    assert.doesNotMatch(compact, /data-entry="terminal"/, phase)
  }
})
