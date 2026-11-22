// Path arithmetic for the remote path bar. The host resolves paths against
// its home; the page resolves relative paths and `..` against the folder the
// user is looking at, which is what a file manager does.
import test from 'node:test'
import assert from 'node:assert/strict'

import {
  normalizeRemotePath,
  resolveRemotePath,
} from './file-transfer.ts'

test('absolute paths keep their prefix and collapse . and ..', () => {
  assert.equal(normalizeRemotePath('/home/me/../you/./docs'), '/home/you/docs')
  assert.equal(normalizeRemotePath('C:\\Users\\me\\..\\you'), 'C:/Users/you')
  assert.equal(normalizeRemotePath('//server/share/./x'), '//server/share/x')
  // `..` cannot climb above the root.
  assert.equal(normalizeRemotePath('/../../etc'), '/etc')
})

test('a relative path and .. resolve against the current folder', () => {
  assert.equal(
    resolveRemotePath('docs', '/home/me'),
    '/home/me/docs',
  )
  assert.equal(
    resolveRemotePath('..', '/home/me/docs'),
    '/home/me',
  )
  assert.equal(
    resolveRemotePath('../pics', '/home/me/docs'),
    '/home/me/pics',
  )
})

test('an absolute path ignores the current folder', () => {
  assert.equal(
    resolveRemotePath('/srv/data', '/home/me'),
    '/srv/data',
  )
  assert.equal(
    resolveRemotePath('D:\\work', '/home/me'),
    'D:/work',
  )
})

test('~ is expanded from the host-reported home', () => {
  assert.equal(resolveRemotePath('~', '/home/me', '/home/me'), '/home/me')
  assert.equal(
    resolveRemotePath('~/Documents', '/home/me', '/home/me'),
    '/home/me/Documents',
  )
  // Without a known root, a home-relative path stays relative to the host
  // home, which is exactly where the host resolves it.
  assert.equal(resolveRemotePath('~/Documents', '', ''), 'Documents')
})

test('empty input opens the host home', () => {
  assert.equal(resolveRemotePath('', '/home/me', '/home/me'), '/home/me')
  assert.equal(resolveRemotePath('   ', '/home/me', '/home/me'), '/home/me')
})
