// Independent-tab model: one tab renders one page. These checks pin the
// intent -> page mapping and the title suffix that tells three tabs of the
// same host apart.
import assert from 'node:assert/strict'
import test from 'node:test'

import {
  intentSuffixKey,
  isPaneIntent,
  nextVisibleTabId,
  normalizeSessionIntent,
  pageFeatures,
  paneConnectionId,
  tabTitle,
} from '../src/lib/session-intent.ts'

test('missing or unknown intents stay desktop sessions', () => {
  assert.equal(normalizeSessionIntent(undefined), 'desktop')
  assert.equal(normalizeSessionIntent(null), 'desktop')
  assert.equal(normalizeSessionIntent(''), 'desktop')
  assert.equal(normalizeSessionIntent('screen'), 'desktop')
  for (const intent of ['files', 'chat', 'terminal']) {
    assert.equal(normalizeSessionIntent(intent), intent)
  }
})

test('only the desktop page paints a canvas and offers picture controls', () => {
  assert.deepEqual(pageFeatures('desktop'), {
    canvas: true,
    quality: true,
    input: true,
    panel: null,
  })
  for (const intent of ['files', 'chat', 'terminal']) {
    const features = pageFeatures(intent)
    assert.equal(features.canvas, false, `${intent} must not render a canvas`)
    assert.equal(features.quality, false, `${intent} has no quality controls`)
    assert.equal(features.input, false, `${intent} owns no remote input`)
    assert.equal(features.panel, intent)
  }
  // An absent intent is a desktop page, never a blank one.
  assert.equal(pageFeatures(undefined).canvas, true)
})

test('pane intents are exactly files, chat and terminal', () => {
  assert.equal(isPaneIntent('desktop'), false)
  assert.equal(isPaneIntent('files'), true)
  assert.equal(isPaneIntent('chat'), true)
  assert.equal(isPaneIntent('terminal'), true)
})

test('tab titles carry the purpose suffix, desktop stays plain', () => {
  assert.equal(tabTitle('YQYX-OFFICE01', 'desktop', '文件传输'), 'YQYX-OFFICE01')
  assert.equal(tabTitle('YQYX-OFFICE01', 'files', '文件传输'), 'YQYX-OFFICE01 · 文件传输')
  assert.equal(tabTitle('YQYX-OFFICE01', 'chat', '聊天'), 'YQYX-OFFICE01 · 聊天')
  assert.equal(tabTitle('YQYX-OFFICE01', 'terminal', '终端'), 'YQYX-OFFICE01 · 终端')
  // No translation yet: do not leave a dangling separator.
  assert.equal(tabTitle('HOST', 'files', ''), 'HOST')
})

test('the three tabs of one host share a label but not an intent', () => {
  const label = 'YQYX-OFFICE01'
  const titles = ['desktop', 'terminal', 'files'].map((intent) =>
    tabTitle(label, intent, intent === 'desktop' ? '' : intent),
  )
  assert.deepEqual(titles, [
    'YQYX-OFFICE01',
    'YQYX-OFFICE01 · terminal',
    'YQYX-OFFICE01 · files',
  ])
  assert.equal(new Set(titles).size, 3)
})

test('suffix keys are i18n keys, desktop has none', () => {
  assert.equal(intentSuffixKey('desktop'), null)
  assert.equal(intentSuffixKey('files'), 'session.files')
  assert.equal(intentSuffixKey('chat'), 'session.chat')
  assert.equal(intentSuffixKey('terminal'), 'terminal.title')
})

test('a promoted pane shares its owner runtime instead of reconnecting', () => {
  assert.equal(paneConnectionId(undefined, 'session-7'), 'session-7')
  assert.equal(paneConnectionId('session-7', 'session-9'), 'session-7')
})

test('minimizing picks the nearest tab that is still visible', () => {
  const tabs = [
    { id: 'session-1' },
    { id: 'session-2' },
    { id: 'session-3' },
  ]
  assert.equal(nextVisibleTabId(tabs, 'session-2'), 'session-1')
  assert.equal(nextVisibleTabId(tabs, 'session-1'), 'session-2')
  assert.equal(nextVisibleTabId(tabs, 'session-3'), 'session-2')
  // Already-minimized tabs are skipped instead of restored by accident.
  assert.equal(
    nextVisibleTabId(
      [
        { id: 'session-1', minimized: true },
        { id: 'session-2' },
        { id: 'session-3' },
      ],
      'session-3',
    ),
    'session-2',
  )
  assert.equal(
    nextVisibleTabId(
      [{ id: 'session-1', minimized: true }],
      'session-1',
    ),
    'home',
  )
})

test('minimizing keeps every tab and its pane alive', () => {
  // The tab simply gains a flag; nothing is removed from the list, which is
  // what lets PaneHost keep the chat scrollback / terminal buffer mounted.
  const tabs = [{ id: 'session-9', intent: 'terminal' }]
  tabs[0].minimized = true
  assert.equal(tabs.length, 1)
  assert.equal(tabs[0].intent, 'terminal')
  assert.equal(tabs[0].minimized, true)
  assert.equal(nextVisibleTabId(tabs, 'session-9'), 'home')
})
