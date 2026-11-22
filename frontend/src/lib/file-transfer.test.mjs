// Path arithmetic for the remote path bar. The host resolves paths against
// its home; the page resolves relative paths and `..` against the folder the
// user is looking at, which is what a file manager does.
import test from 'node:test'
import assert from 'node:assert/strict'

import {
  dropTarget,
  droppedTree,
  joinPath,
  joinRelative,
  normalizeRemotePath,
  parentPath,
  pathCrumbs,
  pathFlavor,
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

test('joinPath uses the separator of the folder being displayed', () => {
  assert.equal(joinPath('C:\\', 'Windows'), 'C:\\Windows')
  assert.equal(joinPath('C:/Users', 'me'), 'C:/Users/me')
  assert.equal(joinPath('/', 'etc'), '/etc')
  assert.equal(joinPath('', 'x'), 'x')
  assert.equal(joinRelative('C:\\Users\\me', 'a/b'), 'C:\\Users\\me\\a\\b')
  assert.equal(joinRelative('/home/me', 'a/b'), '/home/me/a/b')
})

test('parentPath stops at a volume or share root', () => {
  assert.equal(parentPath('/etc'), '/')
  assert.equal(parentPath('/etc/ssh'), '/etc')
  assert.equal(parentPath('/'), '')
  assert.equal(parentPath('C:\\Windows'), 'C:\\')
  assert.equal(parentPath('C:\\Windows\\System32'), 'C:\\Windows')
  assert.equal(parentPath('C:\\'), '')
  assert.equal(parentPath('\\\\server\\share'), '')
  assert.equal(parentPath('\\\\server\\share\\dir'), '\\\\server\\share')
})

test('pathCrumbs builds clickable prefixes for drives and POSIX roots', () => {
  assert.deepEqual(pathCrumbs('/var/log'), [
    { name: '/', path: '/' },
    { name: 'var', path: '/var' },
    { name: 'log', path: '/var/log' },
  ])
  assert.deepEqual(pathCrumbs('C:\\Users\\me'), [
    { name: 'C:', path: 'C:\\' },
    { name: 'Users', path: 'C:\\Users' },
    { name: 'me', path: 'C:\\Users\\me' },
  ])
  assert.deepEqual(pathCrumbs('\\\\server\\share\\dir'), [
    { name: '\\\\server\\share', path: '\\\\server\\share' },
    { name: 'dir', path: '\\\\server\\share\\dir' },
  ])
  assert.deepEqual(pathCrumbs(''), [])
})

test('pathFlavor tells a Windows path from a POSIX one', () => {
  assert.equal(pathFlavor('C:\\Windows'), 'windows')
  assert.equal(pathFlavor('C:/Windows'), 'windows')
  assert.equal(pathFlavor('\\\\server\\share'), 'windows')
  assert.equal(pathFlavor('/var/log'), 'posix')
  assert.equal(pathFlavor('relative'), 'unknown')
  assert.equal(pathFlavor(''), 'unknown')
})

test('a drive switch parses like any other absolute path', () => {
  assert.equal(normalizeRemotePath('D:\\'), 'D:/')
  assert.equal(resolveRemotePath('D:\\games', 'C:\\Users\\me', 'C:\\Users\\me'),
    'D:/games')
  assert.deepEqual(pathCrumbs('D:\\games'), [
    { name: 'D:', path: 'D:\\' },
    { name: 'games', path: 'D:\\games' },
  ])
})

test('a dropped tree keeps folder names and expands nested folders', () => {
  const tree = droppedTree([
    'a.txt',
    'photos/b.jpg',
    'photos/2024/c.jpg',
    'docs/notes.txt',
  ])
  assert.deepEqual(tree.dirs, ['docs', 'photos', 'photos/2024'])
  assert.deepEqual(tree.selection, [
    { path: 'a.txt', dir: false },
    { path: 'photos', dir: true },
    { path: 'docs', dir: true },
  ])
})

test('a drop lands in the shown folder, else the remembered one', () => {
  assert.equal(dropTarget('/home/me/docs', '/home/me/old'), '/home/me/docs')
  // The roots view has no writable path; the remembered folder is the guess.
  assert.equal(dropTarget('', '/home/me/old'), '/home/me/old')
  assert.equal(dropTarget(null, '  /home/me/old  '), '/home/me/old')
  // Nothing is known: the page must ask instead of guessing.
  assert.equal(dropTarget('', ''), '')
  assert.equal(dropTarget(null, null), '')
})
