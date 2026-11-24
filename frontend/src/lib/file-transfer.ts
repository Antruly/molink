/// Pure planning helpers for the two-pane file-transfer panel.
///
/// The panel deals in three different trees: the local one (a picked folder
/// handle or a `webkitdirectory` FileList), the remote one (one `listing`
/// answer at a time) and the selected subset that is about to move. Keeping
/// the path arithmetic here makes the same rules testable without a DOM and
/// keeps the React component to state management and protocol plumbing.

import type { OffsetStore } from './download-state'

export interface TransferEntry {
  /// Path relative to the local source root, slash separated.
  path: string
  name: string
  dir: boolean
  size: number
}

export interface TransferSourceFile {
  path: string
  size: number
}

export interface UploadSelection {
  /// Path relative to the currently open local directory.
  path: string
  dir: boolean
}

/// Windows reserves these names in every folder, with or without a suffix.
/// Rejecting them here matches the host's check, so the panel can count them
/// as skipped instead of starting a transfer that will fail.
const RESERVED_NAMES = new Set([
  'CON', 'PRN', 'AUX', 'NUL',
  'COM1', 'COM2', 'COM3', 'COM4', 'COM5', 'COM6', 'COM7', 'COM8', 'COM9',
  'LPT1', 'LPT2', 'LPT3', 'LPT4', 'LPT5', 'LPT6', 'LPT7', 'LPT8', 'LPT9',
])

export function isReservedName(segment: string): boolean {
  const trimmed = segment.replace(/[ .]+$/g, '')
  if (trimmed === '') return false
  const stem = trimmed.split('.')[0]!.toUpperCase()
  return RESERVED_NAMES.has(stem)
}

/// Slash-separated, no leading `./`, no duplicate or trailing separators.
export function normalizeRelativePath(raw: string): string {
  return raw
    .replace(/\\/g, '/')
    .replace(/^(\.\/)+/, '')
    .replace(/\/+/g, '/')
    .replace(/\/+$/, '')
}

/// Lexically normalise a path that may be absolute, keeping its drive/UNC
/// prefix. `..` cannot climb above the root. Output always uses `/`; the host
/// accepts that on Windows too.
export function normalizeRemotePath(raw: string): string {
  const forward = raw.replace(/\\/g, '/')
  const drive = /^[A-Za-z]:/.exec(forward)?.[0] ?? ''
  const rest = drive === '' ? forward : forward.slice(2)
  const unc = rest.startsWith('//')
  const absolute = rest.startsWith('/')
  const parts: string[] = []
  for (const segment of rest.split('/')) {
    if (segment === '' || segment === '.') continue
    if (segment === '..') {
      if (parts.length > 0 && parts[parts.length - 1] !== '..') parts.pop()
      else if (!absolute && !unc) parts.push('..')
      continue
    }
    parts.push(segment)
  }
  const prefix = drive + (unc ? '//' : absolute ? '/' : '')
  const out = prefix + parts.join('/')
  return out === '' ? '.' : out
}

/// Which path grammar a string uses. The panel needs this before it offers to
/// make the other side "jump to the same path": a Windows host cannot open a
/// POSIX path and vice versa.
export type PathFlavor = 'windows' | 'posix' | 'unknown'

export function pathFlavor(path: string): PathFlavor {
  if (path === '') return 'unknown'
  if (/^[A-Za-z]:[\\/]/.test(path)) return 'windows'
  if (/^[\\/]{2}/.test(path)) return 'windows'
  if (path.includes('\\')) return 'windows'
  if (path.startsWith('/')) return 'posix'
  return 'unknown'
}

/// Joins a name onto a displayed folder path using that path's separator, so
/// a Windows host never receives a mixed `C:\Users/me` path.
export function joinPath(base: string, name: string): string {
  if (base === '') return name
  const separator = base.includes('\\') ? '\\' : '/'
  const trimmed = base.replace(/[\\/]+$/, '')
  if (trimmed === '') return separator + name
  return trimmed + separator + name
}

/// The parent shown by the up button. A volume root (and a UNC share root)
/// has no parent, which the remote panel turns into its drive list.
export function parentPath(path: string): string {
  if (path === '') return ''
  const separator = path.includes('\\') ? '\\' : '/'
  const text = path.replace(/[\\/]+$/, '')
  if (text === '') return ''
  // UNC share root: \\server\share has no browsable parent.
  if (/^[\\/]{2}[^\\/]+[\\/][^\\/]+$/.test(text)) return ''
  const cut = Math.max(text.lastIndexOf('/'), text.lastIndexOf('\\'))
  if (cut < 0) return ''
  const parent = text.slice(0, cut)
  if (parent === '') return separator
  if (/^[A-Za-z]:$/.test(parent)) return parent + separator
  return parent
}

export interface PathCrumb {
  name: string
  path: string
}

/// Clickable breadcrumb prefixes. The volume/drive root is the first crumb;
/// the roots view itself (`''`) has no crumbs.
export function pathCrumbs(path: string): PathCrumb[] {
  if (path === '') return []
  const separator = path.includes('\\') ? '\\' : '/'
  const flavor = pathFlavor(path)
  if (flavor === 'windows' && /^[\\/]{2}/.test(path)) {
    const parts = path.split(/[\\/]+/).filter(Boolean)
    if (parts.length < 2) return []
    const out: PathCrumb[] = []
    let current = `\\\\${parts[0]}\\${parts[1]}`
    out.push({ name: current, path: current })
    for (let index = 2; index < parts.length; index += 1) {
      current += separator + parts[index]
      out.push({ name: parts[index]!, path: current })
    }
    return out
  }
  if (flavor === 'windows') {
    const drive = /^[A-Za-z]:/.exec(path)?.[0]
    if (!drive) return []
    const out: PathCrumb[] = [{ name: drive, path: drive + separator }]
    const parts = path.slice(2).split(/[\\/]+/).filter(Boolean)
    let current = drive + separator
    for (const part of parts) {
      current += part
      out.push({ name: part, path: current })
      current += separator
    }
    return out
  }
  if (path.startsWith('/')) {
    const out: PathCrumb[] = [{ name: '/', path: '/' }]
    const parts = path.split('/').filter(Boolean)
    let current = ''
    for (const part of parts) {
      current += '/' + part
      out.push({ name: part, path: current })
    }
    return out
  }
  return []
}

/// Joins a '/'-separated relative upload path onto a native absolute folder.
export function joinRelative(base: string, relative: string): string {
  const separator = base.includes('\\') ? '\\' : '/'
  const parts = relative.split(/[\\/]+/).filter(Boolean)
  if (parts.length === 0) return base
  return joinPath(base, parts.join(separator))
}


/// Resolve what the user typed into the remote path box.
///
/// Empty means the host's home. `~` / `~/x` are expanded against @p root, the
/// host's reported home; the host itself does not expand a tilde. Absolute
/// paths (POSIX, Windows drive or UNC) are normalised as-is; relative paths
/// and `.`/`..` are resolved against the currently displayed folder, which is
/// what a user expects when editing the path bar.
export function resolveRemotePath(
  text: string,
  base: string,
  root = '',
): string {
  const raw = text.trim()
  if (raw === '') return root
  const home = root.trim()
  if (raw === '~') return home
  if (raw.startsWith('~/') || raw.startsWith('~\\')) {
    const rest = raw.slice(2)
    const joined = home === '' ? rest : `${home}/${rest}`
    return normalizeRemotePath(joined)
  }
  const drive = /^[A-Za-z]:[\\/]/.test(raw)
  const unc = /^[\\/]{2}/.test(raw)
  const absolute = raw.startsWith('/') || drive || unc
  if (!absolute && base === '') return normalizeRemotePath(raw)
  return normalizeRemotePath(absolute ? raw : `${base}/${raw}`)
}

/// Whether a path may be sent to the host as an upload name. Absolute paths
/// (POSIX or UNC), Windows drive letters, traversal and reserved names are
/// refused, exactly like `file_transfer::parse_relative_path` on the host.
export function isSafeRelativePath(raw: string): boolean {
  const text = normalizeRelativePath(raw)
  if (text === '') return false
  if (text.startsWith('/')) return false
  if (/^[A-Za-z]:/.test(text)) return false
  return text
    .split('/')
    .every((segment) =>
      segment !== '' && segment !== '.' && segment !== '..' &&
      !isReservedName(segment))
}

/// Segments safe to write under a directory handle. Unlike an upload name,
/// a name read from the remote host is sanitized instead of rejected: the
/// data already exists on the other side and losing a character is better
/// than dropping the file. `..` and absolute prefixes never survive.
export function safeTargetSegments(
  raw: string,
  fallback: string,
): string[] {
  const out: string[] = []
  for (const part of raw.replace(/\\/g, '/').split('/')) {
    if (part === '' || part === '.' || part === '..') continue
    const cleaned = part
      // Control characters are what we are stripping.
      // eslint-disable-next-line no-control-regex
      .replace(/[\\/:*?"<>|\u0000-\u001f\u007f]/g, '_')
      .trim()
    if (cleaned === '' || cleaned === '.' || cleaned === '..') continue
    out.push(cleaned)
  }
  if (out.length === 0) out.push(fallback)
  return out
}

/// Immediate children of `current` in a flat local tree. Directories sort
/// before files; names are compared case-insensitively like the host listing.
export function localEntriesAt(
  files: TransferSourceFile[],
  dirs: string[],
  current: string,
): TransferEntry[] {
  const base = normalizeRelativePath(current)
  const prefix = base === '' ? '' : base + '/'
  const byName = new Map<string, TransferEntry>()
  const add = (path: string, dir: boolean, size: number) => {
    const text = normalizeRelativePath(path)
    if (!text.startsWith(prefix)) return
    const rest = text.slice(prefix.length)
    if (rest === '') return
    const cut = rest.indexOf('/')
    const name = cut === -1 ? rest : rest.slice(0, cut)
    if (name === '') return
    const entry = byName.get(name)
    if (entry) {
      if (cut !== -1 || dir) {
        entry.dir = true
        entry.size = 0
      } else {
        entry.size += size
      }
      return
    }
    byName.set(name, {
      path: prefix + name,
      name,
      dir: cut !== -1 || dir,
      size: cut !== -1 || dir ? 0 : size,
    })
  }
  for (const dir of dirs) add(dir, true, 0)
  for (const file of files) add(file.path, false, file.size)
  return [...byName.values()].sort((a, b) => {
    if (a.dir !== b.dir) return a.dir ? -1 : 1
    return a.name.toLowerCase() < b.name.toLowerCase() ? -1 : 1
  })
}

/// A drop's flat path list turned into the tree `buildUploadPlan` expects.
/// Dropping a folder keeps its name as the first segment (`photos/a.jpg`), so
/// the remote folder gets the same `photos/` tree. Several dropped roots are
/// selected individually; a top-level file is selected as a file.
export function droppedTree(paths: string[]): {
  dirs: string[]
  selection: UploadSelection[]
} {
  const dirs = new Set<string>()
  const tops = new Map<string, boolean>()
  for (const raw of paths) {
    const path = normalizeRelativePath(raw)
    if (path === '') continue
    const parts = path.split('/')
    for (let index = 1; index < parts.length; index += 1) {
      dirs.add(parts.slice(0, index).join('/'))
    }
    tops.set(parts[0]!, parts.length > 1 || tops.get(parts[0]!) === true)
  }
  return {
    dirs: [...dirs].sort(),
    selection: [...tops.entries()].map(([path, dir]) => ({ path, dir })),
  }
}

/// Where a drop on the session lands. The folder the panel is showing wins;
/// with no panel/listing yet, the last folder the user browsed is the only
/// sensible guess. An empty result means the page must ask once.
export function dropTarget(
  listingPath: string | null | undefined,
  remembered: string | null | undefined,
): string {
  const current = (listingPath ?? '').trim()
  if (current !== '') return current
  return (remembered ?? '').trim()
}

/// The plan for one "send over" action: the selected files plus every
/// selected folder expanded recursively, and the empty folders that have no
/// file to carry them. Paths are relative to the open local directory, which
/// is exactly what the host expects in `offer.name` and `mkdir.path`.
export function buildUploadPlan<T extends TransferSourceFile>(
  files: T[],
  dirs: string[],
  selection: UploadSelection[],
): { files: T[]; dirs: string[]; totalBytes: number; skipped: number } {
  const byPath = new Map<string, T>()
  for (const file of files) {
    const path = normalizeRelativePath(file.path)
    if (!isSafeRelativePath(path)) continue
    byPath.set(path, file)
  }
  const allDirs = new Set<string>()
  for (const dir of dirs) {
    const path = normalizeRelativePath(dir)
    if (isSafeRelativePath(path)) allDirs.add(path)
  }
  const chosenFiles = new Map<string, T>()
  const chosenDirs = new Set<string>()
  let skipped = 0
  for (const item of selection) {
    const path = normalizeRelativePath(item.path)
    if (path === '' || !isSafeRelativePath(path)) {
      skipped += 1
      continue
    }
    const isDir = item.dir || allDirs.has(path)
    if (isDir) {
      if (!allDirs.has(path)) allDirs.add(path)
      chosenDirs.add(path)
      for (const [filePath, file] of byPath) {
        if (filePath.startsWith(path + '/')) chosenFiles.set(filePath, file)
      }
      for (const dir of allDirs) {
        if (dir.startsWith(path + '/')) chosenDirs.add(dir)
      }
    } else if (byPath.has(path)) {
      chosenFiles.set(path, byPath.get(path)!)
    } else {
      skipped += 1
    }
  }
  // A folder that will contain a file is created by `offer` itself; only the
  // folders with nothing under them need an explicit `mkdir`.
  const emptyDirs = [...chosenDirs]
    .filter((dir) =>
      ![...chosenFiles.keys()].some((file) => file.startsWith(dir + '/')))
    .sort()
  // Return the normalized path: the host rejects `a//b`, and the selection
  // may have come from a hand-typed or drop-derived path.
  const outFiles = [...chosenFiles.entries()]
    .sort((a, b) => (a[0] < b[0] ? -1 : 1))
    .map(([path, file]) => ({ ...file, path }))
  return {
    files: outFiles,
    dirs: emptyDirs,
    totalBytes: outFiles.reduce((sum, file) => sum + file.size, 0),
    skipped,
  }
}

export type ChildStatus = 'queued' | 'running' | 'done' | 'error' | 'cancelled'

export interface ProgressChild {
  size: number
  progress: number
  status: ChildStatus
}

export interface AggregateProgress {
  totalFiles: number
  doneFiles: number
  failedFiles: number
  cancelledFiles: number
  totalBytes: number
  doneBytes: number
  percent: number
}

export function aggregateProgress(
  children: ProgressChild[],
): AggregateProgress {
  let doneFiles = 0
  let failedFiles = 0
  let cancelledFiles = 0
  let totalBytes = 0
  let doneBytes = 0
  for (const child of children) {
    totalBytes += child.size
    if (child.status === 'done') {
      doneFiles += 1
      doneBytes += child.size
    } else if (child.status === 'error') {
      failedFiles += 1
      doneBytes += Math.min(child.progress, child.size)
    } else if (child.status === 'cancelled') {
      cancelledFiles += 1
    } else {
      doneBytes += Math.min(child.progress, child.size)
    }
  }
  const percent =
    totalBytes === 0
      ? doneFiles + failedFiles + cancelledFiles === children.length &&
          children.length > 0
        ? 100
        : 0
      : Math.min(100, Math.round((doneBytes / totalBytes) * 100))
  return {
    totalFiles: children.length,
    doneFiles,
    failedFiles,
    cancelledFiles,
    totalBytes,
    doneBytes,
    percent,
  }
}

export type TaskStatus = 'queued' | 'running' | 'done' | 'error' | 'cancelled'

/// A task is finished when no child is still queued or running. Cancellation
/// wins over individual failures so a cancelled task does not look broken.
export function taskStatusFromChildren(
  children: ProgressChild[],
  cancelled: boolean,
): TaskStatus {
  if (children.some((child) =>
    child.status === 'queued' || child.status === 'running')) {
    return 'running'
  }
  if (cancelled || children.some((child) => child.status === 'cancelled')) {
    return 'cancelled'
  }
  if (children.some((child) => child.status === 'error')) return 'error'
  return 'done'
}

export type TransferBlock =
  | 'no-selection'
  | 'no-target'
  | 'no-source'
  | 'busy'
  | 'unsupported'

/// "Send over" needs a local selection, a picked local source and a remote
/// folder that has answered a listing.
export function uploadBlockReason(args: {
  selected: number
  remotePath: string
  hasSource: boolean
  busy: boolean
}): TransferBlock | null {
  if (args.busy) return 'busy'
  if (args.selected === 0) return 'no-selection'
  if (!args.hasSource) return 'no-source'
  if (args.remotePath.trim() === '') return 'no-target'
  return null
}

/// "Receive back" needs a remote selection and a writable local folder.
/// `mode` and `hasTarget` are optional so callers that only know the selection
/// (and the existing tests) keep the original behaviour; the panel passes
/// both, which is what disables the button in a files-only browser and when no
/// local folder handle has been picked yet.
export function downloadBlockReason(args: {
  selected: number
  busy: boolean
  mode?: DownloadMode
  hasTarget?: boolean
}): TransferBlock | null {
  if (args.busy) return 'busy'
  if (args.selected === 0) return 'no-selection'
  if (args.mode === 'files-only') return 'unsupported'
  if (args.hasTarget === false) return 'no-target'
  return null
}

/// A host that predates folder uploads / a custom destination answers an
/// unknown file action with an `error` frame instead of a `file` frame. The
/// client forwards those codes as a synthetic file error so the panel can warn
/// and keep sending the files it can.
export function isUnsupportedAction(
  code?: string | null,
  message?: string | null,
): boolean {
  if (code === 'bad_file_action' || code === 'bad_action' ||
      code === 'unknown_action') {
    return true
  }
  return /unknown (file )?action|unsupported (file )?action/i.test(
    message ?? '')
}

/// One finished task's counters. `skipped` covers selection entries that were
/// refused before a transfer started (unsafe names, files that vanished).
export interface TaskSummary {
  done: number
  failed: number
  cancelled: number
  skipped: number
  total: number
}

export function summarizeTask(
  children: ProgressChild[],
  skipped = 0,
): TaskSummary {
  let done = 0
  let failed = 0
  let cancelled = 0
  for (const child of children) {
    if (child.status === 'done') done += 1
    else if (child.status === 'error') failed += 1
    else if (child.status === 'cancelled') cancelled += 1
  }
  return {
    done,
    failed,
    cancelled,
    skipped,
    total: children.length + skipped,
  }
}

/// Maps an absolute remote path (Windows or POSIX) to the slash-separated
/// path below `base`. Returns null when `path` is not inside `base`, and `''`
/// for `base` itself, so callers can build a local tree without ever joining
/// an absolute path or a `..`.
export function relativeUnder(base: string, path: string): string | null {
  const clean = (value: string) => value.replace(/\\/g, '/').replace(/\/+$/, '')
  const root = clean(base)
  const full = clean(path)
  if (root === '') return full
  const caseInsensitive = /^[A-Za-z]:/.test(root) || root.startsWith('//')
  const left = caseInsensitive ? root.toLowerCase() : root
  const right = caseInsensitive ? full.toLowerCase() : full
  if (right === left) return ''
  if (!right.startsWith(left + '/')) return null
  return full.slice(root.length + 1)
}

export type DownloadMode = 'directory' | 'files-only'

export interface DirectoryScope {
  isSecureContext?: boolean
  showSaveFilePicker?: unknown
  showDirectoryPicker?: unknown
}

/// Preserving a remote folder tree needs a writable directory handle, which
/// only the File System Access API offers -- and only on a secure context.
/// Everything else degrades to one Save button per file, structure dropped.
export function downloadModeFor(scope: DirectoryScope): DownloadMode {
  return scope.isSecureContext === true &&
    typeof scope.showSaveFilePicker === 'function' &&
    typeof scope.showDirectoryPicker === 'function'
    ? 'directory'
    : 'files-only'
}

/// Identity of one resumable upload. The host keeps a `.part` keyed by the
/// same logical file: destination folder plus the relative path and size. The
/// host address is included so two different hosts never share an offset.
export function uploadResumeKey(
  host: string,
  destination: string,
  path: string,
  size: number,
): string {
  return [
    'molink.upload.v1',
    encodeURIComponent(host),
    encodeURIComponent(destination),
    encodeURIComponent(path),
    String(size),
  ].join('|')
}

function clampUploadOffset(offset: number, size: number): number {
  if (!Number.isSafeInteger(offset) || offset < 0) return 0
  return offset > size ? size : offset
}

/// Remembers the host-confirmed prefix of an interrupted upload. Zero (or a
/// complete file) clears the entry: there is nothing left to resume then.
export function saveUploadOffset(
  store: OffsetStore,
  key: string,
  offset: number,
  size: number,
): void {
  const value = clampUploadOffset(offset, size)
  try {
    if (value <= 0 || value >= size) {
      store.removeItem(key)
      return
    }
    store.setItem(key, String(value))
  } catch {
    // Storage can be disabled; a manual retry still resumes from what the
    // host reports in its `accept` answer.
  }
}

export function loadUploadOffset(
  store: OffsetStore,
  key: string,
  size: number,
): number {
  let raw: string | null = null
  try {
    raw = store.getItem(key)
  } catch {
    return 0
  }
  if (raw === null) return 0
  const value = Number(raw)
  if (!Number.isSafeInteger(value) || value <= 0 || value > size) return 0
  return value
}

export function clearUploadOffset(store: OffsetStore, key: string): void {
  try {
    store.removeItem(key)
  } catch {
    // Nothing to clean up when storage is unavailable.
  }
}

/// Parses `webkitRelativePath` / dropped paths into a flat local source.
/// The first path segment of every entry is the picked folder's name, so the
/// tree root is that name rather than an invisible `''`.
export function sourceFromInputFiles(
  files: Array<{ file: File; path?: string; size?: number }>,
  fallbackRoot = '',
): {
  rootName: string
  files: Array<{ file: File; path: string; size: number }>
  dirs: string[]
} {
  const entries = files
    .map((item) => ({
      file: item.file,
      path: normalizeRelativePath(item.path ?? item.file.name),
      size: item.size ?? item.file.size,
    }))
    .filter((item) => item.path !== '')
  const first = entries[0]?.path ?? ''
  const rootName = first.includes('/')
    ? first.slice(0, first.indexOf('/'))
    : fallbackRoot
  const dirs = new Set<string>()
  if (rootName !== '') dirs.add(rootName)
  for (const entry of entries) {
    const parts = entry.path.split('/')
    for (let index = 1; index < parts.length; index += 1) {
      dirs.add(parts.slice(0, index).join('/'))
    }
  }
  return { rootName, files: entries, dirs: [...dirs].filter(Boolean) }
}
