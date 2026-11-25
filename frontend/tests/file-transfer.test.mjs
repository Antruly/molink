import assert from 'node:assert/strict'
import test from 'node:test'
import {
  aggregateProgress,
  buildUploadPlan,
  downloadBlockReason,
  downloadModeFor,
  isReservedName,
  isSafeRelativePath,
  isUnsupportedAction,
  localEntriesAt,
  normalizeRelativePath,
  relativeUnder,
  remoteEntryPath,
  safeTargetSegments,
  sourceFromInputFiles,
  summarizeTask,
  taskStatusFromChildren,
  uploadBlockReason,
} from '../src/lib/file-transfer.ts'

test('relative paths are normalized and unsafe ones rejected', () => {
  assert.equal(normalizeRelativePath('a\\b\\c.txt'), 'a/b/c.txt')
  assert.equal(normalizeRelativePath('./a//b/'), 'a/b')
  assert.equal(isSafeRelativePath('photos/2024/a.jpg'), true)
  assert.equal(isSafeRelativePath('照片/旅行/海.jpg'), true)
  assert.equal(isSafeRelativePath('a.txt'), true)
  assert.equal(isSafeRelativePath('/etc/passwd'), false)
  assert.equal(isSafeRelativePath('\\\\server\\share\\x'), false)
  assert.equal(isSafeRelativePath('C:\\Windows\\x'), false)
  assert.equal(isSafeRelativePath('c:/temp/x'), false)
  assert.equal(isSafeRelativePath('../up'), false)
  assert.equal(isSafeRelativePath('a/../../b'), false)
  assert.equal(normalizeRelativePath('a//b'), 'a/b')
  assert.equal(isSafeRelativePath(normalizeRelativePath('a//b')), true)
  assert.equal(isSafeRelativePath(''), false)
})

test('reserved device names are recognised with or without a suffix', () => {
  for (const name of ['CON', 'con', 'CON.txt', 'nul', 'Com1.log', 'LPT9']) {
    assert.equal(isReservedName(name), true, name)
  }
  for (const name of ['console', 'COM0', 'LPT10', 'auxiliary', 'a.txt']) {
    assert.equal(isReservedName(name), false, name)
  }
})

test('local listing shows one level of a flat tree', () => {
  const files = [
    { path: 'photos/2024/a.jpg', size: 10 },
    { path: 'photos/2024/b.jpg', size: 20 },
    { path: 'photos/notes.txt', size: 5 },
  ]
  const dirs = ['photos', 'photos/2024', 'photos/empty']
  const root = localEntriesAt(files, dirs, 'photos')
  assert.deepEqual(
    root.map((entry) => [entry.name, entry.dir]),
    [['2024', true], ['empty', true], ['notes.txt', false]],
  )
  const nested = localEntriesAt(files, dirs, 'photos/2024')
  assert.equal(nested.length, 2)
  assert.equal(nested[1].size, 20)
  assert.equal(localEntriesAt(files, dirs, '').length, 1)
})

test('a folder selection expands recursively and keeps empty folders', () => {
  const files = [
    { path: 'photos/2024/a.jpg', size: 10 },
    { path: 'photos/2024/b.jpg', size: 20 },
    { path: 'photos/notes.txt', size: 5 },
    { path: 'other/c.txt', size: 7 },
  ]
  const dirs = ['photos', 'photos/2024', 'photos/empty', 'other']
  const plan = buildUploadPlan(files, dirs, [
    { path: 'photos', dir: true },
  ])
  assert.deepEqual(
    plan.files.map((file) => file.path),
    ['photos/2024/a.jpg', 'photos/2024/b.jpg', 'photos/notes.txt'],
  )
  assert.deepEqual(plan.dirs, ['photos/empty'])
  assert.equal(plan.totalBytes, 35)
  assert.equal(plan.skipped, 0)

  const single = buildUploadPlan(files, dirs, [
    { path: 'photos/2024/b.jpg', dir: false },
  ])
  assert.deepEqual(single.files.map((file) => file.path), ['photos/2024/b.jpg'])
  assert.deepEqual(single.dirs, [])
  assert.equal(single.totalBytes, 20)
})

test('an empty folder is sent as a mkdir, unsafe names are skipped', () => {
  const plan = buildUploadPlan([], ['photos', 'photos/empty'], [
    { path: 'photos', dir: true },
  ])
  assert.deepEqual(plan.files, [])
  assert.deepEqual(plan.dirs, ['photos', 'photos/empty'])
  assert.equal(plan.totalBytes, 0)

  const skipped = buildUploadPlan(
    [{ path: '../secret', size: 1 }, { path: 'ok.txt', size: 2 }],
    [],
    [{ path: '../secret', dir: false }, { path: 'missing.txt', dir: false }],
  )
  assert.deepEqual(skipped.files.map((file) => file.path), [])
  assert.equal(skipped.skipped, 2)
})

test('overlapping selections are de-duplicated', () => {
  const plan = buildUploadPlan(
    [{ path: 'a/b.txt', size: 3 }],
    ['a'],
    [{ path: 'a', dir: true }, { path: 'a/b.txt', dir: false }],
  )
  assert.deepEqual(plan.files.map((file) => file.path), ['a/b.txt'])
  assert.equal(plan.totalBytes, 3)
})

test('aggregate progress counts files, bytes and failures', () => {
  const children = [
    { size: 100, progress: 100, status: 'done' },
    { size: 100, progress: 50, status: 'running' },
    { size: 50, progress: 0, status: 'queued' },
    { size: 20, progress: 20, status: 'error' },
  ]
  const progress = aggregateProgress(children)
  assert.equal(progress.totalFiles, 4)
  assert.equal(progress.doneFiles, 1)
  assert.equal(progress.failedFiles, 1)
  assert.equal(progress.totalBytes, 270)
  assert.equal(progress.doneBytes, 170)
  assert.equal(progress.percent, 63)

  const done = aggregateProgress([
    { size: 0, progress: 0, status: 'done' },
    { size: 0, progress: 0, status: 'done' },
  ])
  assert.equal(done.percent, 100)

  assert.equal(
    taskStatusFromChildren(children, false),
    'running',
  )
  assert.equal(
    taskStatusFromChildren(
      [{ size: 1, progress: 0, status: 'cancelled' },
       { size: 1, progress: 1, status: 'done' }],
      true,
    ),
    'cancelled',
  )
  assert.equal(
    taskStatusFromChildren(
      [{ size: 1, progress: 1, status: 'error' }],
      false,
    ),
    'error',
  )
  assert.equal(
    taskStatusFromChildren(
      [{ size: 1, progress: 1, status: 'done' }],
      false,
    ),
    'done',
  )
})

test('the action buttons explain why they are disabled', () => {
  assert.equal(
    uploadBlockReason({
      selected: 0, remotePath: '/home/u', hasSource: true, busy: false,
    }),
    'no-selection',
  )
  assert.equal(
    uploadBlockReason({
      selected: 1, remotePath: '', hasSource: true, busy: false,
    }),
    'no-target',
  )
  assert.equal(
    uploadBlockReason({
      selected: 1, remotePath: '/home/u', hasSource: false, busy: false,
    }),
    'no-source',
  )
  assert.equal(
    uploadBlockReason({
      selected: 1, remotePath: '/home/u', hasSource: true, busy: false,
    }),
    null,
  )
  assert.equal(downloadBlockReason({ selected: 0, busy: false }), 'no-selection')
  assert.equal(downloadBlockReason({ selected: 2, busy: false }), null)
})

test('a remote page without the directory picker drops the structure', () => {
  const secure = {
    isSecureContext: true,
    showSaveFilePicker: () => undefined,
    showDirectoryPicker: () => undefined,
  }
  assert.equal(downloadModeFor(secure), 'directory')
  assert.equal(downloadModeFor({ ...secure, isSecureContext: false }), 'files-only')
  assert.equal(downloadModeFor({ isSecureContext: true }), 'files-only')
})

test('the receive button knows about the browser and the local target', () => {
  assert.equal(
    downloadBlockReason({
      selected: 1, busy: false, mode: 'files-only', hasTarget: true,
    }),
    'unsupported',
  )
  assert.equal(
    downloadBlockReason({
      selected: 1, busy: false, mode: 'directory', hasTarget: false,
    }),
    'no-target',
  )
  assert.equal(
    downloadBlockReason({
      selected: 1, busy: false, mode: 'directory', hasTarget: true,
    }),
    null,
  )
  assert.equal(
    downloadBlockReason({
      selected: 0, busy: false, mode: 'files-only', hasTarget: false,
    }),
    'no-selection',
  )
  assert.equal(
    downloadBlockReason({
      selected: 1, busy: true, mode: 'directory', hasTarget: true,
    }),
    'busy',
  )
})

test('a remote path maps to a relative local tree on both separators', () => {
  assert.equal(
    relativeUnder('/home/u/mLT', '/home/u/mLT/sub1/a.txt'),
    'sub1/a.txt',
  )
  assert.equal(
    relativeUnder('C:\\Users\\A\\mLT', 'C:\\Users\\A\\mLT\\sub1\\a.txt'),
    'sub1/a.txt',
  )
  assert.equal(
    relativeUnder('C:\\Users\\A\\mLT', 'c:\\users\\a\\mlt\\sub1\\a.txt'),
    'sub1/a.txt',
  )
  assert.equal(relativeUnder('/home/u/mLT', '/home/u/mLT'), '')
  assert.equal(relativeUnder('/home/u/mLT', '/home/u/other.txt'), null)
  assert.equal(relativeUnder('/home/u/mLT', '/home/u/mLT2/a.txt'), null)
  assert.equal(relativeUnder('', '/home/u/a.txt'), '/home/u/a.txt')
})

test('an old host is recognised from its file-action error', () => {
  assert.equal(isUnsupportedAction('bad_file_action', ''), true)
  assert.equal(isUnsupportedAction('', 'unknown file action: mkdir'), true)
  assert.equal(isUnsupportedAction('bad_action', ''), true)
  assert.equal(isUnsupportedAction('', 'could not create the folder'), false)
  assert.equal(isUnsupportedAction(undefined, undefined), false)
})

test('task summaries keep failures, cancellations and skips apart', () => {
  const summary = summarizeTask([
    { size: 10, progress: 10, status: 'done' },
    { size: 10, progress: 4, status: 'error' },
    { size: 10, progress: 0, status: 'cancelled' },
    { size: 10, progress: 2, status: 'running' },
  ], 2)
  assert.deepEqual(summary, {
    done: 1,
    failed: 1,
    cancelled: 1,
    skipped: 2,
    total: 6,
  })
  assert.deepEqual(summarizeTask([], 0), {
    done: 0,
    failed: 0,
    cancelled: 0,
    skipped: 0,
    total: 0,
  })
})

test('a selected remote folder keeps its own name in the local target', () => {
  const base = 'C:\\Users\\A\\mLT'
  const selected = { name: 'sub1', path: `${base}\\sub1` }
  const child = `${selected.path}\\sub2\\照片.jpg`
  const rel = relativeUnder(base, child)
  assert.equal(rel, 'sub1/sub2/照片.jpg')
  assert.deepEqual(safeTargetSegments(rel, 'file'), [
    'sub1', 'sub2', '照片.jpg',
  ])
  assert.equal(relativeUnder(base, selected.path), 'sub1')
})

test('remote names are sanitized into safe local path segments', () => {
  assert.deepEqual(safeTargetSegments('photos/2024/a.jpg', 'file'), [
    'photos', '2024', 'a.jpg',
  ])
  assert.deepEqual(safeTargetSegments('../../etc/passwd', 'file'), [
    'etc', 'passwd',
  ])
  assert.deepEqual(safeTargetSegments('a:b/c?.txt', 'file'), [
    'a_b', 'c_.txt',
  ])
  assert.deepEqual(safeTargetSegments('', 'file'), ['file'])
})

test('a pathless listing row is rebuilt from its folder, never unknown.bin', () => {
  // Regular listings stop sending absolute paths; the displayed folder is
  // still an absolute host path, so the entry can be reconstructed exactly.
  assert.equal(
    remoteEntryPath('/home/u/docs', { name: 'a.txt' }),
    '/home/u/docs/a.txt',
  )
  assert.equal(
    remoteEntryPath('C:\\Users\\u\\docs', { name: 'a.txt' }),
    'C:\\Users\\u\\docs\\a.txt',
  )
  // A roots row carries its own path, which wins over the synthetic join.
  assert.equal(
    remoteEntryPath('', { name: 'C:', path: 'C:\\' }),
    'C:\\',
  )
  // No invented fallback name: an unusable row yields '' so the caller can
  // report the path problem instead of writing a file called unknown.bin.
  assert.equal(remoteEntryPath('/home/u', { name: '  ' }), '')
})

test('a webkitdirectory FileList becomes a rooted flat tree', () => {
  const make = (name, size) => ({
    file: { name, size },
    path: `photos/2024/${name}`,
  })
  const source = sourceFromInputFiles([make('a.jpg', 3), make('b.jpg', 4)])
  assert.equal(source.rootName, 'photos')
  assert.deepEqual(source.files.map((entry) => entry.path), [
    'photos/2024/a.jpg', 'photos/2024/b.jpg',
  ])
  assert.deepEqual(source.dirs, ['photos', 'photos/2024'])
})
