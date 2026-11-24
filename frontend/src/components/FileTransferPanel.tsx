import {
  ArrowDownToLine,
  ArrowLeft,
  ArrowLeftRight,
  ArrowRight,
  ChevronUp,
  ClipboardCopy,
  ClipboardPaste,
  Download,
  File as FileIcon,
  Folder,
  FolderOpen,
  Home,
  Link2,
  RefreshCw,
  Save,
  Upload,
  X,
} from 'lucide-react'
import {
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
  type DragEvent,
  type PointerEvent as ReactPointerEvent,
} from 'react'
import { useTranslation } from 'react-i18next'
import type { TFunction } from 'i18next'
import {
  coreBaseUrl,
  fetchLocalDirectory,
  makeLocalDirectory,
  moveLocalFile,
  readLocalFileRange,
  removeLocalFile,
  writeLocalFileChunk,
  type FsEntry,
  type FsRoot,
} from '../lib/api'
import { clipboardFilesMode } from '../lib/clipboard-local'
import { directoryPickMode } from '../lib/directory-pick'
import {
  collectDropEntries,
  onDroppedFiles,
  onRemoteDropFiles,
  type StagedFile,
} from '../lib/file-drop'
import {
  buildRemoteDragPayload,
  dragItemsFor,
  encodeRemoteDragPayload,
  isOutsideWindow,
  REMOTE_DRAG_MIME,
  type RemoteDragItem,
} from '../lib/remote-drag'
import {
  onShellDirectoryPicked,
  requestShellDirectoryPick,
  shellDirectoryPickerCapable,
  shellDragOutCapable,
  startShellDragOut,
} from '../lib/shell'
import type {
  ClipboardFileEntry,
  FileEntry,
  FileMessage,
} from '../lib/client'
import type { SessionConnection } from '../lib/session-connection'
import {
  acceptDownloadChunk,
  clearDownloadOffset,
  downloadComplete,
  downloadResumeKey,
  type OffsetStore,
} from '../lib/download-state'
import {
  canPickDirectory,
  canUseFileSystemAccess,
  isPickerCancellation,
  maySaveFromButton,
  suggestedFileName,
  supportsDirectoryInput,
  type SaveStrategy,
} from '../lib/download-save'
// ---------------------------------------------------------------- upload acks
//
// The file channel has a byte budget: past it the proxy drops chunks and the
// host sees a gap, which it can only report as "out of order". The viewer
// therefore keeps a credit window and waits for `file-ack` before running
// further ahead; a missing ack rewinds to the last confirmed byte.
const kUploadWindowBytes = 2 * 1024 * 1024
const kUploadAckTimeoutMs = 2000

interface UploadAck {
  confirmed: number
  failed: boolean
  waiters: Array<(reached: boolean) => void>
  wait: (ms: number) => Promise<boolean>
}

const uploadAcks = new Map<number, UploadAck>()

/// Upload id -> resume key, so a `file-ack` can persist the confirmed prefix
/// without threading the key through every message handler.
const uploadAckKeys = new Map<number, { key: string; size: number }>()

function uploadAckFor(id: number): UploadAck {
  const existing = uploadAcks.get(id)
  if (existing) return existing
  const ack: UploadAck = {
    confirmed: 0,
    failed: false,
    waiters: [],
    wait(ms) {
      return new Promise<boolean>((resolve) => {
        let settled = false
        const timer = window.setTimeout(() => {
          if (settled) return
          settled = true
          ack.waiters = ack.waiters.filter((w) => w !== waiter)
          resolve(false)
        }, ms)
        const waiter = (reached: boolean) => {
          if (settled) return
          settled = true
          window.clearTimeout(timer)
          resolve(reached)
        }
        ack.waiters.push(waiter)
      })
    },
  }
  uploadAcks.set(id, ack)
  return ack
}

function settleUploadAcks(id: number, received: number, failed: boolean) {
  const ack = uploadAcks.get(id)
  if (!ack) return
  ack.confirmed = Math.max(ack.confirmed, received)
  ack.failed = ack.failed || failed
  const waiters = ack.waiters
  ack.waiters = []
  for (const wake of waiters) wake(true)
}

import {
  aggregateProgress,
  buildUploadPlan,
  clearUploadOffset,
  downloadBlockReason,
  downloadModeFor,
  droppedTree,
  dropTarget,
  isUnsupportedAction,
  joinPath,
  joinRelative,
  loadUploadOffset,
  localEntriesAt,
  normalizeRelativePath,
  parentPath,
  pathCrumbs,
  pathFlavor,
  relativeUnder,
  resolveRemotePath,
  safeTargetSegments,
  saveUploadOffset,
  sourceFromInputFiles,
  summarizeTask,
  taskStatusFromChildren,
  uploadBlockReason,
  uploadResumeKey,
  type ChildStatus,
  type DownloadMode,
  type TransferBlock,
} from '../lib/file-transfer'
import { useAppStore } from '../store/app'

const downloadTimeoutMs = 30_000
const browseTimeoutMs = 15_000
const uploadReplyTimeoutMs = 30_000
const uploadDoneTimeoutMs = 90_000
/// How long an interrupted upload waits for the session to come back before
/// it gives up. The client retries a dead socket for about a minute and then
/// once a minute, so five minutes covers a short host restart.
const uploadReconnectMs = 5 * 60_000

const LOCAL_PATH_KEY = 'molink.files.localPath'
const REMOTE_PATH_KEY = 'molink.files.remotePath'

/// localStorage-backed offset bookkeeping. Storage can be unavailable (private
/// mode, a dropped quota), so every access is best-effort.
const resumeStore: OffsetStore = {
  getItem(key) {
    try {
      return window.localStorage.getItem(key)
    } catch {
      return null
    }
  },
  setItem(key, value) {
    try {
      window.localStorage.setItem(key, value)
    } catch {
      // A lost offset only means the next attempt starts from the host's
      // `.part` size instead of the locally remembered one.
    }
  },
  removeItem(key) {
    try {
      window.localStorage.removeItem(key)
    } catch {
      // Nothing to remove.
    }
  },
}

/// Waits for the session socket to be live again. Returns false on timeout or
/// when the user cancelled the transfer while waiting.
async function waitUntilLive(
  isLive: () => boolean,
  timeoutMs: number,
  cancelled: () => boolean,
): Promise<boolean> {
  const deadline = Date.now() + timeoutMs
  for (;;) {
    if (cancelled()) return false
    if (isLive()) return true
    if (Date.now() >= deadline) return false
    await new Promise<void>((resolve) => window.setTimeout(resolve, 250))
  }
}

function readStored(key: string): string {
  try {
    return window.localStorage.getItem(key) ?? ''
  } catch {
    return ''
  }
}

function storePath(key: string, value: string): void {
  try {
    window.localStorage.setItem(key, value)
  } catch {
    // Storage can be disabled; remembering the path is only a convenience.
  }
}

function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`
  if (bytes < 1024 * 1024 * 1024) {
    return `${(bytes / 1024 / 1024).toFixed(1)} MB`
  }
  return `${(bytes / 1024 / 1024 / 1024).toFixed(2)} GB`
}

/// A path the user intends as an absolute home path rather than a subfolder
/// of the currently picked source: POSIX `/x`, Windows `C:\x`, UNC `\\x`, or
/// a `~`-relative home path.
function looksAbsolutePath(text: string): boolean {
  return (
    text === '~' ||
    /^~[\\/]/.test(text) ||
    text.startsWith('/') ||
    /^[\\/]{2}/.test(text) ||
    /^[A-Za-z]:[\\/]/.test(text)
  )
}

function errorText(error: unknown): string {
  if (error instanceof Error && error.message !== '') return error.message
  return typeof error === 'string' ? error : ''
}

/// The host's optional write sandbox refused a path. It sends the allowed
/// roots so the page can name the limit instead of showing a bare "failed".
function isForbiddenPath(message: FileMessage): boolean {
  return message.action === 'forbidden_path' ||
    message.code === 'forbidden_path'
}

function sandboxDeniedText(t: TFunction, message: FileMessage): string {
  const roots = message.allowed_roots ?? []
  return t('files.sandboxDenied', {
    roots: roots.length > 0 ? roots.join(' · ') : '~/moLink',
  })
}

interface RemoteListing {
  /// Empty while the host is showing its volume/root list.
  path: string
  root: string
  parent: string
  entries: FileEntry[]
  roots: FsRoot[]
}

interface BrowseRequest {
  path: string
  display: boolean
  resolve: (listing: RemoteListing) => void
  reject: (error: Error) => void
  timer: number
}

interface LocalFile {
  path: string
  size: number
  /// Present when the browser picked/dropped the file. A folder listed by the
  /// local core has no `File`; its bytes are read from `absPath`.
  file: File | null
  absPath?: string
}

type LocalSource =
  | { kind: 'handle'; root: FileSystemDirectoryHandle; rootName: string }
  | {
      kind: 'snapshot'
      rootName: string
      /// Where the snapshot starts; "up" never goes above it. A dropped set
      /// of several folders starts at the invisible root, a picked folder
      /// starts at that folder's name.
      basePath: string
      files: LocalFile[]
      dirs: string[]
      label: string
    }
  | {
      /// A folder listed by the local core (`/api/fs/list`). The desktop core
      /// can also read/write these bytes (`/api/fs/read` `/api/fs/write`), so
      /// it behaves like a real folder rather than a names-only preview.
      kind: 'fs'
      path: string
      parent: string
      root: string
      roots: FsRoot[]
    }

interface HandleEntry {
  name: string
  dir: boolean
  size: number
  handle: FileSystemHandle
}

interface HandleLevel {
  name: string
  handle: FileSystemDirectoryHandle
}

interface LocalRow {
  key: string
  name: string
  rel: string
  dir: boolean
  size: number
  handle: FileSystemHandle | null
  file: File | null
}

interface TaskChild {
  key: string
  name: string
  path?: string
  size: number
  progress: number
  status: ChildStatus
  error?: string
}

interface TransferTask {
  key: number
  direction: 'up' | 'down'
  name: string
  children: TaskChild[]
  cancelled: boolean
  finished: boolean
  phase: 'preparing' | 'running' | 'done' | 'error' | 'cancelled'
  speed: number
  note?: string
  skipped: number
  dirsTotal: number
  dirsDone: number
  dirsFailed: number
  remotePaths: string[]
}

interface UploadAnswer {
  ok: boolean
  cancelled?: boolean
  id?: number
  chunk?: number
  dir?: string
  path?: string
  message?: string
  code?: string
  /// On `accept`: the host's real contiguous offset. Older hosts omit it.
  offset?: number
}

interface UploadWaiter {
  kind: 'offer' | 'done' | 'mkdir'
  resolve: (answer: UploadAnswer) => void
  timer: number
}

interface UploadRun {
  taskKey: number
  cancelled: boolean
  activeId: number
}

interface DownloadRun {
  taskKey: number
  cancelled: boolean
}

type DownloadPhase = 'waiting' | 'transferring' | 'saving' | 'saved'

interface DownloadJob {
  key: number
  path: string
  name: string
  taskKey: number | null
  childKey: string | null
  silent: boolean
  strategy: SaveStrategy
  target: Promise<FileSystemWritableFileStream> | null
  /// Directory-mode target: the writable is created under this handle.
  baseDir: FileSystemDirectoryHandle | null
  segments: string[] | null
  /// Local-core target (desktop shell): bytes are written to `fsTemp`, then
  /// renamed to `fsTarget` when the transfer completes.
  fsTarget: string | null
  fsTemp: string | null
  /// Final absolute path the local core chose (`fsTarget`, possibly with a
  /// ` (1)` suffix). Drag-out reports these to the shell.
  finalPath: string | null
  /// Called once the job settles: the committed path, or null on failure.
  onDone: ((path: string | null) => void) | null
  /// Browser drag-out fallback: hand the completed blob to the browser as a
  /// download instead of waiting for a Save button click.
  autoDownload: boolean
  writable: FileSystemWritableFileStream | null
  parentDir: FileSystemDirectoryHandle | null
  createdFile: boolean
  requesting: boolean
  id: number
  size: number
  /// Remote mtime, part of the resume key. 0 when the caller did not supply
  /// one (a dropped set, the clipboard queue).
  mtime: number
  /// Key of the persisted offset, filled in when `sending` names the size.
  resumeKey: string
  /// The socket went away mid-transfer; keep the job and re-request it once
  /// the session is live instead of failing it.
  waitingLive: boolean
  /// When the offset was last written to storage.
  lastPersist: number
  received: number
  eofSeen: boolean
  cancelled: boolean
  finishing: boolean
  committed: boolean
  chunks: Uint8Array[]
  writeChain: Promise<void>
  writeError: unknown
  timer: number
  resolve: () => void
}

interface ReadySave {
  key: number
  name: string
  size: number
  blob: Blob
}

interface RemotePlanFile {
  path: string
  rel: string
  size: number
  mtime?: number
}

/// The shape `buildUploadPlan` returns; named here because both the snapshot
/// and the live folder-handle paths fill one in.
interface UploadPlan {
  files: LocalFile[]
  dirs: string[]
  totalBytes: number
  skipped: number
}

interface DirectoryValues {
  values(): AsyncIterableIterator<FileSystemHandle>
}

async function* directoryHandles(
  dir: FileSystemDirectoryHandle,
): AsyncIterableIterator<FileSystemHandle> {
  const iterable = dir as unknown as DirectoryValues
  if (typeof iterable.values !== 'function') return
  yield* iterable.values()
}

async function readDirectory(
  dir: FileSystemDirectoryHandle,
): Promise<HandleEntry[]> {
  const entries: HandleEntry[] = []
  for await (const handle of directoryHandles(dir)) {
    if (handle.kind === 'directory') {
      entries.push({ name: handle.name, dir: true, size: 0, handle })
      continue
    }
    let size = 0
    try {
      size = (await (handle as FileSystemFileHandle).getFile()).size
    } catch {
      size = 0
    }
    entries.push({ name: handle.name, dir: false, size, handle })
  }
  return entries.sort((a, b) => {
    if (a.dir !== b.dir) return a.dir ? -1 : 1
    return a.name.toLowerCase() < b.name.toLowerCase() ? -1 : 1
  })
}

async function ensureDirectory(
  base: FileSystemDirectoryHandle,
  segments: string[],
): Promise<FileSystemDirectoryHandle> {
  let dir = base
  for (const segment of segments) {
    dir = await dir.getDirectoryHandle(segment, { create: true })
  }
  return dir
}

/// Opens the save dialog while the click's transient activation is still
/// live. The returned promise must not be awaited before the caller returns,
/// or Chrome rejects it with "must be handling a user gesture".
function pickSaveTarget(name: string): {
  strategy: SaveStrategy
  target: Promise<FileSystemWritableFileStream> | null
} {
  if (!canUseFileSystemAccess(window)) {
    return { strategy: 'button', target: null }
  }
  try {
    const handle = window.showSaveFilePicker!({ suggestedName: name })
    return {
      strategy: 'picker',
      target: handle.then((file) => file.createWritable()),
    }
  } catch {
    return { strategy: 'button', target: null }
  }
}

/// "Receive all" gets one directory choice instead of one dialog per file.
function pickDirectoryTargets(
  names: string[],
): Promise<FileSystemWritableFileStream>[] | null {
  if (!canPickDirectory(window) || names.length === 0) return null
  try {
    const directory = window.showDirectoryPicker!({ mode: 'readwrite' })
    return names.map((name) =>
      directory.then(async (dir) => {
        const handle = await dir.getFileHandle(name, { create: true })
        return handle.createWritable()
      }),
    )
  } catch {
    return null
  }
}

function blockText(
  t: TFunction,
  block: TransferBlock,
  direction: 'up' | 'down',
): string {
  switch (block) {
    case 'no-selection':
      return t('files.noSelection')
    case 'no-target':
      return direction === 'up'
        ? t('files.needRemoteTarget')
        : t('files.needLocalTarget')
    case 'no-source':
      return t('files.needLocalSource')
    case 'unsupported':
      return t('files.unsupportedDownload')
    case 'busy':
      return t('files.busy')
  }
}

function rootKindText(t: TFunction, kind: string): string {
  switch (kind) {
    case 'removable':
      return t('files.driveRemovable')
    case 'network':
      return t('files.driveNetwork')
    case 'cdrom':
      return t('files.driveCdrom')
    case 'ram':
      return t('files.driveRam')
    case 'home':
      return t('files.driveHome')
    case 'unknown':
      return t('files.driveUnknown')
    default:
      return ''
  }
}

function rootLabel(t: TFunction, root: FsRoot): string {
  const kind = rootKindText(t, root.kind)
  return kind === '' ? root.name : `${root.name} · ${kind}`
}

function downloadPhaseText(
  phase: DownloadPhase,
  t: TFunction,
): string {
  if (phase === 'saved') return t('files.saved')
  if (phase === 'saving') return t('files.saving')
  if (phase === 'waiting') return t('files.loading')
  return t('files.sending')
}

/// File transfer drawer: a local tree on the left, the remote host's home on
/// the right and one explicit action in the middle. Uploads carry relative
/// paths (folders keep their shape), downloads rebuild the same tree through a
/// directory handle when the browser allows it.
export function FileTransferPanel({
  open,
  onClose,
  connection,
  onRemotePathChange,
  onRequestOpen,
}: {
  open: boolean
  onClose: () => void
  connection: SessionConnection | null
  /// Publishes the folder the remote pane is showing so the session surface
  /// can describe a drop target.
  onRemotePathChange?: (path: string) => void
  /// Asks the session to open this drawer (a drop with no destination yet).
  onRequestOpen?: () => void
}) {
  const { t } = useTranslation()
  const localMode = useAppStore((state) => state.localMode)
  const token = useAppStore((state) => state.token)
  const autoClipboardFiles = useAppStore((state) => state.remoteClipboardFiles)
  const clipboardMode = clipboardFilesMode(
    localMode,
    window.location.origin,
    coreBaseUrl(),
    autoClipboardFiles,
  )

  // ---------------------------------------------------------------- tasks
  const [tasks, setTasks] = useState<TransferTask[]>([])
  const tasksRef = useRef<TransferTask[]>([])
  const nextKey = useRef(1)
  const childSeq = useRef(1)
  const paintAt = useRef<Map<string, number>>(new Map())
  const speedSamples = useRef<Map<number, {
    time: number
    bytes: number
    speed: number
  }>>(new Map())

  const publishTasks = useCallback((next: TransferTask[]) => {
    tasksRef.current = next
    setTasks(next)
  }, [])

  const updateTask = useCallback(
    (key: number, change: (task: TransferTask) => TransferTask) => {
      publishTasks(
        tasksRef.current.map((task) => (task.key === key ? change(task) : task)),
      )
    },
    [publishTasks],
  )

  const patchChild = useCallback(
    (taskKey: number, childKey: string, patch: Partial<TaskChild>) => {
      updateTask(taskKey, (task) => ({
        ...task,
        children: task.children.map((child) =>
          child.key === childKey ? { ...child, ...patch } : child,
        ),
      }))
    },
    [updateTask],
  )

  const noteTaskSpeed = useCallback(
    (taskKey: number) => {
      const task = tasksRef.current.find((item) => item.key === taskKey)
      if (!task) return
      const bytes = aggregateProgress(task.children).doneBytes
      const now = performance.now()
      const sample = speedSamples.current.get(taskKey)
      if (!sample) {
        speedSamples.current.set(taskKey, { time: now, bytes, speed: 0 })
        return
      }
      const elapsed = (now - sample.time) / 1000
      if (elapsed < 0.5) return
      const instant = Math.max(0, (bytes - sample.bytes) / elapsed)
      const speed = sample.speed > 0
        ? sample.speed * 0.65 + instant * 0.35
        : instant
      speedSamples.current.set(taskKey, { time: now, bytes, speed })
      updateTask(taskKey, (item) => ({ ...item, speed }))
    },
    [updateTask],
  )

  const patchProgress = useCallback(
    (
      taskKey: number,
      childKey: string,
      progress: number,
      force = false,
    ) => {
      const now = performance.now()
      const key = `${taskKey}:${childKey}`
      if (!force && now - (paintAt.current.get(key) ?? 0) < 120) return
      paintAt.current.set(key, now)
      patchChild(taskKey, childKey, { progress, status: 'running' })
      noteTaskSpeed(taskKey)
    },
    [noteTaskSpeed, patchChild],
  )

  const appendNote = useCallback(
    (taskKey: number, note: string) => {
      updateTask(taskKey, (task) =>
        task.note?.includes(note)
          ? task
          : { ...task, note: task.note ? `${task.note} · ${note}` : note },
      )
    },
    [updateTask],
  )

  const createTask = useCallback(
    (
      direction: 'up' | 'down',
      name: string,
      children: TaskChild[],
      options: { skipped?: number; dirsTotal?: number } = {},
    ): number => {
      const key = nextKey.current++
      const task: TransferTask = {
        key,
        direction,
        name,
        children,
        cancelled: false,
        finished: false,
        phase: 'preparing',
        speed: 0,
        skipped: options.skipped ?? 0,
        dirsTotal: options.dirsTotal ?? 0,
        dirsDone: 0,
        dirsFailed: 0,
        remotePaths: [],
      }
      publishTasks([...tasksRef.current, task].slice(-24))
      return key
    },
    [publishTasks],
  )

  const finalizeTask = useCallback(
    (taskKey: number) => {
      speedSamples.current.delete(taskKey)
      updateTask(taskKey, (task) => {
        const fallback: ChildStatus = task.cancelled ? 'cancelled' : 'error'
        const children = task.children.map((child) =>
          child.status === 'queued' || child.status === 'running'
            ? { ...child, status: fallback }
            : child,
        )
        const status = taskStatusFromChildren(children, false)
        // A cancel that arrives after every byte was sent cannot undo a host
        // that has already renamed the upload. In that case the children are
        // all `done` (or every folder was created) and the task is done, even
        // though the user pressed cancel.
        const units = children.length + task.dirsTotal
        const settled = children.filter((child) =>
          child.status === 'done' || child.status === 'error' ||
          child.status === 'cancelled').length +
          task.dirsDone + task.dirsFailed
        const stopped = task.cancelled &&
          (children.some((child) => child.status === 'cancelled') ||
            settled < units || units === 0)
        const phase: TransferTask['phase'] =
          stopped || status === 'cancelled'
            ? 'cancelled'
            : status === 'error' || task.dirsFailed > 0
              ? 'error'
              : 'done'
        return { ...task, children, finished: true, phase, speed: 0 }
      })
    },
    [updateTask],
  )

  const removeTask = useCallback(
    (taskKey: number) => {
      publishTasks(tasksRef.current.filter((task) => task.key !== taskKey))
    },
    [publishTasks],
  )

  const clearFinished = useCallback(() => {
    publishTasks(tasksRef.current.filter((task) => !task.finished))
  }, [publishTasks])

  // ------------------------------------------------------- local pane state
  const [source, setSource] = useState<LocalSource | null>(null)
  const [handleEntries, setHandleEntries] = useState<HandleEntry[]>([])
  const [handleLevels, setHandleLevels] = useState<HandleLevel[]>([])
  const [snapshotPath, setSnapshotPath] = useState('')
  const [localInput, setLocalInput] = useState(() =>
    readStored(LOCAL_PATH_KEY))
  const [localSelection, setLocalSelection] = useState<Set<string>>(
    () => new Set(),
  )
  const [localLoading, setLocalLoading] = useState(false)
  const [localNote, setLocalNote] = useState<string | null>(null)
  /// Rows listed through the local core's `/api/fs/list`.
  const [fsEntries, setFsEntries] = useState<FsEntry[]>([])
  const localLoadToken = useRef(0)
  const localInitial = useRef<string | null>(null)
  const directoryInputRef = useRef<HTMLInputElement>(null)
  const fileInputRef = useRef<HTMLInputElement>(null)

  // ------------------------------------------------------ remote pane state
  const [listing, setListing] = useState<RemoteListing | null>(null)
  const listingRef = useRef<RemoteListing | null>(null)
  const [remoteInput, setRemoteInput] = useState(() =>
    readStored(REMOTE_PATH_KEY))
  const [remoteSelection, setRemoteSelection] = useState<Map<string, FileEntry>>(
    () => new Map(),
  )
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const browseQueue = useRef<BrowseRequest[]>([])
  const browseActive = useRef<BrowseRequest | null>(null)
  const browsePumpRef = useRef<() => void>(() => undefined)
  const finishBrowseRef = useRef<(listing: RemoteListing | null, error?: string) => void>(
    () => undefined,
  )

  useEffect(() => {
    listingRef.current = listing
  }, [listing])

  useEffect(() => {
    if (listing) storePath(REMOTE_PATH_KEY, listing.path)
  }, [listing])

  // ------------------------------------------------------------ connection
  const connectionRef = useRef<SessionConnection | null>(connection)
  useEffect(() => {
    connectionRef.current = connection
  }, [connection])

  const [uploadBusy, setUploadBusy] = useState(false)
  const [bulkBusy, setBulkBusy] = useState(false)

  // ------------------------------------------------------------- clipboard
  const [clipboardFiles, setClipboardFiles] = useState<ClipboardFileEntry[]>([])
  const [clipboardNote, setClipboardNote] = useState<string | null>(null)

  // ----------------------------------------------------------- browse queue
  const finishBrowse = useCallback(
    (result: RemoteListing | null, failure?: string) => {
      const active = browseActive.current
      if (!active) {
        if (result) {
          listingRef.current = result
          setListing(result)
          setRemoteInput(result.path)
        } else if (failure) {
          setError(failure)
        }
        return
      }
      window.clearTimeout(active.timer)
      browseActive.current = null
      if (result) {
        active.resolve(result)
        if (active.display) {
          listingRef.current = result
          setListing(result)
          setRemoteInput(result.path)
          setRemoteSelection(new Map())
          setError(null)
          setLoading(false)
        }
      } else {
        const message = failure ?? t('files.error')
        active.reject(new Error(message))
        if (active.display) {
          setLoading(false)
          setError(message)
        }
      }
      browsePumpRef.current()
    },
    [t],
  )
  // Published after commit: a ref read by an event handler must not be
  // written during render (React Compiler).
  useEffect(() => {
    finishBrowseRef.current = finishBrowse
  })

  const pumpBrowse = useCallback(() => {
    if (browseActive.current) return
    const request = browseQueue.current.shift()
    if (!request) return
    browseActive.current = request
    request.timer = window.setTimeout(() => {
      if (browseActive.current === request) {
        finishBrowseRef.current(null, t('files.timeout'))
      }
    }, browseTimeoutMs)
    connectionRef.current?.sendFileAction('browse', { path: request.path })
  }, [t])
  useEffect(() => {
    browsePumpRef.current = pumpBrowse
  })

  const requestListing = useCallback(
    (path: string, display: boolean): Promise<RemoteListing> =>
      new Promise((resolve, reject) => {
        browseQueue.current.push({
          path,
          display,
          resolve,
          reject,
          timer: 0,
        })
        pumpBrowse()
      }),
    [pumpBrowse],
  )

  const browseRemote = useCallback(
    (path: string) => {
      setLoading(true)
      setError(null)
      void requestListing(path.trim() === '' ? '' : path.trim(), true).catch(
        () => undefined,
      )
    },
    [requestListing],
  )

  /// Resolve the path bar the same way a file manager does: absolute paths
  /// and `~` go to the host as typed, relative paths and `..` are resolved
  /// against the folder currently shown. The host then canonicalises again and
  /// refuses anything outside the user's home.
  const openRemotePath = useCallback(() => {
    browseRemote(
      resolveRemotePath(remoteInput, listing?.path ?? '', listing?.root ?? ''),
    )
  }, [browseRemote, listing, remoteInput])

  // ---------------------------------------------------------- upload waiter
  const uploadWaiter = useRef<UploadWaiter | null>(null)

  const resolveUpload = useCallback((answer: UploadAnswer): boolean => {
    const waiter = uploadWaiter.current
    if (!waiter) return false
    window.clearTimeout(waiter.timer)
    uploadWaiter.current = null
    waiter.resolve(answer)
    return true
  }, [])

  const waitUpload = useCallback(
    (kind: UploadWaiter['kind'], timeout: number): Promise<UploadAnswer> =>
      new Promise((resolve) => {
        const current = uploadWaiter.current
        if (current) {
          window.clearTimeout(current.timer)
          current.resolve({ ok: false, message: t('files.timeout') })
        }
        const timer = window.setTimeout(() => {
          if (uploadWaiter.current?.timer === timer) {
            uploadWaiter.current = null
            resolve({ ok: false, message: t('files.timeout') })
          }
        }, timeout)
        uploadWaiter.current = { kind, resolve, timer }
      }),
    [t],
  )

  // -------------------------------------------------------- download queue
  const downloadSection = useRef<DownloadJob | null>(null)
  const downloadQueue = useRef<DownloadJob[]>([])
  const downloadPumpRef = useRef<() => void>(() => undefined)
  const finishDownloadRef = useRef<
    (job: DownloadJob, failure?: string, cancelled?: boolean) => void
  >(() => undefined)
  const [saveReady, setSaveReady] = useState<ReadySave[]>([])
  const [download, setDownload] = useState<{
    key: number
    phase: DownloadPhase
    name: string
    received: number
    size: number
  } | null>(null)

  const armDownloadTimeout = useCallback(() => {
    const job = downloadSection.current
    if (!job) return
    window.clearTimeout(job.timer)
    const tick = () => {
      if (downloadSection.current !== job || job.finishing) return
      const connection = connectionRef.current
      if (connection && !connection.isLive()) {
        // The session is retrying the socket. The bytes already written stay
        // in place and the reconnect poll below re-requests the file, so do
        // not fail a transfer that is only waiting for the link.
        job.waitingLive = true
        job.timer = window.setTimeout(tick, downloadTimeoutMs)
        return
      }
      finishDownloadRef.current(job, t('files.incomplete'))
    }
    job.timer = window.setTimeout(tick, downloadTimeoutMs)
  }, [t])

  const discardPartial = useCallback(async (job: DownloadJob) => {
    if (job.committed) return
    try {
      await job.writeChain
    } catch {
      // A failed write is the reason we are discarding.
    }
    if (job.writable) {
      try {
        await job.writable.abort()
      } catch {
        // Already closed or aborted.
      }
      job.writable = null
    }
    if (job.createdFile && job.parentDir) {
      try {
        await job.parentDir.removeEntry(job.name)
      } catch {
        // It may already be gone (or the abort removed it).
      }
    }
    if (job.fsTemp && token !== '') {
      try {
        await removeLocalFile(token, job.fsTemp)
      } catch {
        // The staging file may never have been created.
      }
      job.fsTemp = null
    }
  }, [token])

  const finishDownload = useCallback(
    (job: DownloadJob, failure?: string, cancelled = false) => {
      if (job.finishing) return
      job.finishing = true
      window.clearTimeout(job.timer)
      if (job.resumeKey !== '') clearDownloadOffset(resumeStore, job.resumeKey)
      if (downloadSection.current === job) downloadSection.current = null

      const settle = async () => {
        if (failure !== undefined) {
          await discardPartial(job)
          if (job.id !== 0) {
            connectionRef.current?.sendFileAction('cancel', { id: job.id })
          }
          if (job.taskKey !== null && job.childKey) {
            patchChild(job.taskKey, job.childKey, {
              status: cancelled ? 'cancelled' : 'error',
              error: cancelled ? undefined : failure,
            })
            noteTaskSpeed(job.taskKey)
          }
          if (!job.silent) setError(failure)
          setDownload(null)
        } else if (job.fsTemp && job.fsTarget) {
          setDownload((current) =>
            current?.key === job.key
              ? { ...current, phase: 'saving' }
              : current,
          )
          try {
            await job.writeChain
            if (job.writeError !== undefined) throw job.writeError
            if (token === '') throw new Error('no local core token')
            job.finalPath = await moveLocalFile(
              token,
              job.fsTemp,
              job.fsTarget,
            )
            job.fsTemp = null
            job.committed = true
            if (job.taskKey !== null && job.childKey) {
              patchChild(job.taskKey, job.childKey, {
                status: 'done',
                progress: job.size,
                size: job.size,
              })
              noteTaskSpeed(job.taskKey)
            }
            setDownload(null)
          } catch {
            await discardPartial(job)
            if (job.taskKey !== null && job.childKey) {
              patchChild(job.taskKey, job.childKey, {
                status: 'error',
                error: t('files.saveFailed'),
              })
            }
            if (!job.silent) setError(t('files.saveFailed'))
            setDownload(null)
          }
        } else if (job.writable) {
          setDownload((current) =>
            current?.key === job.key
              ? { ...current, phase: 'saving' }
              : current,
          )
          try {
            await job.writeChain
            if (job.writeError !== undefined) throw job.writeError
            await job.writable.close()
            job.committed = true
            if (job.taskKey !== null && job.childKey) {
              patchChild(job.taskKey, job.childKey, {
                status: 'done',
                progress: job.size,
                size: job.size,
              })
              noteTaskSpeed(job.taskKey)
            }
            if (!job.taskKey) {
              setDownload((current) =>
                current?.key === job.key
                  ? { ...current, phase: 'saved' }
                  : current,
              )
              window.setTimeout(() => {
                setDownload((current) =>
                  current?.key === job.key ? null : current,
                )
              }, 2_000)
            } else {
              setDownload(null)
            }
          } catch {
            await discardPartial(job)
            if (job.taskKey !== null && job.childKey) {
              patchChild(job.taskKey, job.childKey, {
                status: 'error',
                error: t('files.saveFailed'),
              })
            }
            if (!job.silent) setError(t('files.saveFailed'))
            setDownload(null)
          }
        } else if (maySaveFromButton(job.strategy, job, false)) {
          const blob = new Blob(job.chunks as BlobPart[])
          if (job.autoDownload) {
            // Drag-out fallback in a plain browser: the bytes arrived after
            // the drop gesture, so this is the closest thing to "download it
            // to the browser's folder". A blocked automatic download still
            // leaves the same blob in the Save list below.
            const url = URL.createObjectURL(blob)
            const anchor = document.createElement('a')
            anchor.href = url
            anchor.download = suggestedFileName(job.name, job.name)
            document.body.appendChild(anchor)
            anchor.click()
            anchor.remove()
            window.setTimeout(() => URL.revokeObjectURL(url), 30_000)
          }
          // If the browser blocks the automatic download, the Save entry is
          // still there; the same check feeds the manual button.
          setSaveReady((current) => [
            ...current.slice(-7),
            { key: job.key, name: job.name, size: job.size, blob },
          ])
          if (job.taskKey !== null && job.childKey) {
            patchChild(job.taskKey, job.childKey, {
              status: 'done',
              progress: job.size,
              size: job.size,
            })
          }
          setDownload(null)
        } else {
          await discardPartial(job)
          if (job.taskKey !== null && job.childKey) {
            patchChild(job.taskKey, job.childKey, {
              status: 'error',
              error: t('files.incomplete'),
            })
          }
          if (!job.silent) setError(t('files.incomplete'))
          setDownload(null)
        }
        job.onDone?.(job.finalPath)
        job.resolve()
        downloadPumpRef.current()
      }
      void settle()
    },
    [discardPartial, noteTaskSpeed, patchChild, t, token],
  )
  useEffect(() => {
    finishDownloadRef.current = finishDownload
  })

  const pumpDownloads = useCallback(() => {
    if (downloadSection.current) return
    const job = downloadQueue.current.shift()
    if (!job) return
    downloadSection.current = job

    const start = async () => {
      if (job.cancelled) {
        await discardPartial(job)
        if (job.taskKey !== null && job.childKey) {
          patchChild(job.taskKey, job.childKey, { status: 'cancelled' })
        }
        job.resolve()
        downloadPumpRef.current()
        return
      }

      if (job.baseDir && job.segments) {
        try {
          const parent = await ensureDirectory(job.baseDir, job.segments)
          job.parentDir = parent
          let existed = true
          try {
            await parent.getFileHandle(job.name)
          } catch {
            existed = false
          }
          const handle = await parent.getFileHandle(job.name, { create: true })
          job.writable = await handle.createWritable()
          job.createdFile = !existed
        } catch {
          finishDownloadRef.current(job, t('files.saveFailed'))
          return
        }
      } else if (job.target) {
        try {
          job.writable = await job.target
        } catch (pickError) {
          if (isPickerCancellation(pickError)) {
            job.cancelled = true
            job.finishing = true
            window.clearTimeout(job.timer)
            if (downloadSection.current === job) downloadSection.current = null
            if (job.taskKey !== null && job.childKey) {
              patchChild(job.taskKey, job.childKey, { status: 'cancelled' })
            }
            job.resolve()
            downloadPumpRef.current()
            return
          }
          // The API exists but this origin/permission refused it; the memory
          // path still delivers the file.
          job.strategy = 'button'
          job.target = null
        }
      }

      if (job.cancelled || downloadSection.current !== job) {
        await discardPartial(job)
        job.resolve()
        downloadPumpRef.current()
        return
      }

      if (job.taskKey !== null) {
        updateTask(job.taskKey, (task) => ({ ...task, phase: 'running' }))
        if (job.childKey) {
          patchChild(job.taskKey, job.childKey, { status: 'running' })
        }
      }
      setDownload({
        key: job.key,
        phase: 'waiting',
        name: job.name,
        received: 0,
        size: job.size,
      })
      job.requesting = true
      connectionRef.current?.sendFileAction('request', { path: job.path })
      armDownloadTimeout()
    }
    void start()
  }, [
    armDownloadTimeout,
    discardPartial,
    patchChild,
    t,
    updateTask,
  ])
  useEffect(() => {
    downloadPumpRef.current = pumpDownloads
  })

  // A dropped session loses the host's download id, but the viewer still has
  // the bytes already written. Once the socket is live again, ask for the same
  // file and let `sending` continue at `job.received`.
  useEffect(() => {
    if (!open) return
    const timer = window.setInterval(() => {
      const job = downloadSection.current
      if (!job || job.finishing || job.cancelled) return
      const connection = connectionRef.current
      if (!connection) return
      if (!connection.isLive()) {
        job.waitingLive = true
        return
      }
      if (!job.waitingLive) return
      job.waitingLive = false
      job.id = 0
      job.requesting = true
      window.clearTimeout(job.timer)
      connection.sendFileAction('request', { path: job.path })
      armDownloadTimeout()
    }, 1000)
    return () => window.clearInterval(timer)
  }, [open, armDownloadTimeout])

  const makeJob = useCallback(
    (props: {
      path: string
      name: string
      taskKey: number | null
      childKey: string | null
      silent: boolean
      strategy: SaveStrategy
      target: Promise<FileSystemWritableFileStream> | null
      baseDir: FileSystemDirectoryHandle | null
      segments: string[] | null
      fsTarget?: string | null
      mtime?: number
      onDone?: (path: string | null) => void
      autoDownload?: boolean
      resolve: () => void
    }): DownloadJob => {
      const key = nextKey.current++
      const fsTarget = props.fsTarget ?? null
      return {
        key,
        ...props,
        fsTarget,
        fsTemp: fsTarget === null ? null : `${fsTarget}.molink-${key}.part`,
        finalPath: null,
        onDone: props.onDone ?? null,
        autoDownload: props.autoDownload === true,
        writable: null,
        parentDir: null,
        createdFile: false,
        requesting: false,
        id: 0,
        size: 0,
        mtime: props.mtime ?? 0,
        resumeKey: '',
        waitingLive: false,
        lastPersist: 0,
        received: 0,
        eofSeen: false,
        cancelled: false,
        finishing: false,
        committed: false,
        chunks: [],
        writeChain: Promise.resolve(),
        writeError: undefined,
        timer: 0,
      }
    },
    [],
  )

  const queueJob = useCallback((job: DownloadJob) => {
    downloadQueue.current.push(job)
    downloadPumpRef.current()
  }, [])

  const downloadRuns = useRef<Map<number, DownloadRun>>(new Map())
  const uploadRuns = useRef<Map<number, UploadRun>>(new Map())

  const runQuickDownloads = useCallback(
    async (
      taskKey: number,
      run: DownloadRun,
      files: Array<{
        path: string
        name: string
        childKey: string
        mtime?: number
      }>,
      targets: Array<Promise<FileSystemWritableFileStream> | null>,
      strategy: SaveStrategy,
      silent: boolean,
      fsTargets: Array<string | null> = [],
    ) => {
      try {
        updateTask(taskKey, (task) => ({ ...task, phase: 'running' }))
        for (let index = 0; index < files.length; index += 1) {
          const file = files[index]!
          if (run.cancelled) {
            patchChild(taskKey, file.childKey, { status: 'cancelled' })
            continue
          }
          await new Promise<void>((resolve) => {
            const job = makeJob({
              path: file.path,
              name: file.name,
              taskKey,
              childKey: file.childKey,
              silent,
              strategy,
              target: targets[index] ?? null,
              baseDir: null,
              segments: null,
              fsTarget: fsTargets[index] ?? null,
              mtime: file.mtime ?? 0,
              resolve,
            })
            queueJob(job)
          })
        }
      } finally {
        downloadRuns.current.delete(taskKey)
        finalizeTask(taskKey)
      }
    },
    [finalizeTask, makeJob, patchChild, queueJob, updateTask],
  )

  const startQuickDownload = useCallback(
    (path: string, name: string, mtime = 0) => {
      const suggested = suggestedFileName(name, path)
      const pick = pickSaveTarget(suggested)
      const childKey = `c${childSeq.current++}`
      const taskKey = createTask('down', suggested, [
        {
          key: childKey,
          name: suggested,
          path,
          size: 0,
          progress: 0,
          status: 'queued',
        },
      ])
      const run: DownloadRun = { taskKey, cancelled: false }
      downloadRuns.current.set(taskKey, run)
      void runQuickDownloads(
        taskKey,
        run,
        [{ path, name: suggested, childKey, mtime }],
        [pick.target],
        pick.strategy,
        false,
      )
    },
    [createTask, runQuickDownloads],
  )

  const startClipboardDownloads = useCallback(() => {
    // The path-based download works only for files the host chose to describe
    // with a path (its own page/shell). A remote viewer gets ids and uses the
    // pull path in remote-clipboard instead.
    const files = clipboardFiles.filter(
      (file): file is ClipboardFileEntry & { path: string } =>
        typeof file.path === 'string' && file.path !== '',
    )
    if (files.length === 0) return
    const names = files.map((file) =>
      suggestedFileName(file.name, file.path))
    const targets = pickDirectoryTargets(names)
    const children = files.map((file, index) => ({
      key: `c${childSeq.current++}`,
      name: names[index]!,
      path: file.path,
      size: file.size,
      progress: 0,
      status: 'queued' as ChildStatus,
    }))
    const taskKey = createTask('down', t('files.clipboardTitle'), children)
    const run: DownloadRun = { taskKey, cancelled: false }
    downloadRuns.current.set(taskKey, run)
    void runQuickDownloads(
      taskKey,
      run,
      files.map((file, index) => ({
        path: file.path,
        name: names[index]!,
        childKey: children[index]!.key,
      })),
      targets ?? files.map(() => null),
      targets ? 'picker' : 'button',
      true,
    )
  }, [clipboardFiles, createTask, runQuickDownloads, t])

  // ----------------------------------------------------------- cancel task
  const cancelTask = useCallback(
    (taskKey: number) => {
      const task = tasksRef.current.find((item) => item.key === taskKey)
      if (!task || task.finished) return
      updateTask(taskKey, (item) => ({
        ...item,
        cancelled: true,
        children: item.children.map((child) =>
          child.status === 'queued' || child.status === 'running'
            ? { ...child, status: 'cancelled' }
            : child,
        ),
      }))

      const upload = uploadRuns.current.get(taskKey)
      if (upload) {
        upload.cancelled = true
        if (upload.activeId !== 0) {
          connectionRef.current?.sendFileAction('cancel', {
            id: upload.activeId,
          })
        }
        const waiter = uploadWaiter.current
        if (waiter) {
          if (waiter.kind === 'done') {
            // All bytes are already with the host. Let its `done`/`cancelled`
            // answer decide whether the file exists; a short fallback still
            // unsticks the task if the answer never comes.
            window.setTimeout(() => {
              if (uploadWaiter.current !== waiter) return
              window.clearTimeout(waiter.timer)
              uploadWaiter.current = null
              waiter.resolve({
                ok: false,
                cancelled: true,
                message: t('files.cancelled'),
              })
            }, 3_000)
          } else {
            window.clearTimeout(waiter.timer)
            uploadWaiter.current = null
            waiter.resolve({
              ok: false,
              cancelled: true,
              message: t('files.cancelled'),
            })
          }
        }
      }

      const downloadRun = downloadRuns.current.get(taskKey)
      if (downloadRun) downloadRun.cancelled = true

      const active = downloadSection.current
      if (active && active.taskKey === taskKey) {
        finishDownloadRef.current(active, t('files.cancelled'), true)
      }
      const kept: DownloadJob[] = []
      for (const queued of downloadQueue.current) {
        if (queued.taskKey !== taskKey) {
          kept.push(queued)
          continue
        }
        // Job records are plain objects owned by this panel, not React state;
        // the pump reads the flag off the same object.
        // eslint-disable-next-line react-hooks/immutability
        queued.cancelled = true
        if (queued.target) {
          void queued.target
            .then((writable) => writable.abort())
            .catch(() => undefined)
        }
        queued.resolve()
      }
      downloadQueue.current = kept
    },
    [t, updateTask],
  )

  // -------------------------------------------------------------- messages
  const handleMessage = useCallback(
    (message: FileMessage) => {
      switch (message.action) {
        case 'accept': {
          const consumed = resolveUpload({
            ok: true,
            id: message.id,
            chunk: message.chunk,
            dir: message.dir,
            offset: message.offset,
          })
          if (!consumed && message.id) {
            const cancelling = [...uploadRuns.current.values()].some(
              (run) => run.cancelled,
            )
            if (cancelling) {
              connectionRef.current?.sendFileAction('cancel', {
                id: message.id,
              })
            }
          }
          break
        }
        case 'file-ack':
          if (message.id) {
            settleUploadAcks(message.id, message.received ?? 0, false)
            // Remember the confirmed prefix so a reconnect resumes here
            // instead of re-sending gigabytes.
            const run = uploadAckKeys.get(message.id)
            if (run) {
              saveUploadOffset(
                resumeStore,
                run.key,
                message.received ?? 0,
                run.size,
              )
            }
          }
          break
        case 'file-nack':
          // The host hit a gap: rewind the window to what it really has and
          // let the upload loop resend from there.
          if (message.id) {
            const ack = uploadAckFor(message.id)
            const reached = message.received ?? 0
            if (reached < ack.confirmed) ack.confirmed = reached
            const waiters = ack.waiters
            ack.waiters = []
            for (const wake of waiters) wake(true)
          }
          break
        case 'reject':
          resolveUpload({ ok: false, message: message.message })
          break
        case 'done':
          resolveUpload({
            ok: message.ok !== false,
            id: message.id,
            path: message.path,
            message: message.message,
          })
          break
        case 'mkdir':
          resolveUpload({
            ok: message.ok !== false,
            path: message.path,
            message: message.message,
          })
          break
        case 'listing':
          if (message.ok === false) {
            finishBrowseRef.current(
              null,
              message.message ?? t('files.error'),
            )
          } else if (message.entries) {
            // `path` is empty for the volume/root list, so test `entries`
            // rather than the path.
            finishBrowseRef.current({
              path: message.path ?? '',
              root: message.root ?? message.path ?? '',
              parent: message.parent ?? parentPath(message.path ?? ''),
              entries: message.entries,
              roots: message.roots ?? [],
            })
          }
          break
        case 'sending': {
          const job = downloadSection.current
          if (!job || !job.requesting || job.id !== 0) return
          job.requesting = false
          const size = message.size
          if (
            message.ok === false ||
            !message.id ||
            typeof size !== 'number' ||
            !Number.isSafeInteger(size) ||
            size < 0
          ) {
            finishDownloadRef.current(
              job,
              isForbiddenPath(message)
                ? sandboxDeniedText(t, message)
                : (message.message ?? t('files.error')),
            )
            return
          }
          // `job.received > 0` means this is the reconnect path: the local
          // writer already holds that prefix, so keep it instead of zeroing
          // the progress and overwriting the file from the start.
          if ((job.size !== 0 && job.size !== size) || job.received > size) {
            finishDownloadRef.current(job, t('files.incomplete'))
            return
          }
          job.id = message.id
          job.size = size
          job.resumeKey = downloadResumeKey(
            connectionRef.current?.targetAddress() ?? '',
            job.path,
            size,
            job.mtime,
          )
          job.name = suggestedFileName(message.name ?? '', job.name)
          if (job.taskKey !== null && job.childKey) {
            patchChild(job.taskKey, job.childKey, {
              size,
              status: 'running',
            })
          }
          setError(null)
          armDownloadTimeout()
          setDownload({
            key: job.key,
            phase: 'transferring',
            name: job.name,
            received: job.received,
            size,
          })
          // An additive field: only ask for an offset once there is one. An
          // old host ignores it and the viewer sees the usual chunk sequence.
          connectionRef.current?.sendFileAction(
            'pull',
            job.received > 0
              ? { id: job.id, offset: job.received }
              : { id: job.id },
          )
          break
        }
        case 'eof': {
          const job = downloadSection.current
          if (!job || (message.id && job.id !== message.id)) return
          if (message.ok === false) {
            finishDownloadRef.current(
              job,
              message.message ?? t('files.error'),
            )
            break
          }
          job.eofSeen = true
          void job.writeChain.then(() => {
            if (job.cancelled || job.finishing) return
            if (job.writeError !== undefined) {
              finishDownloadRef.current(job, t('files.saveFailed'))
              return
            }
            if (!downloadComplete(job)) {
              finishDownloadRef.current(job, t('files.incomplete'))
              return
            }
            finishDownloadRef.current(job)
          })
          break
        }
        case 'cancelled': {
          resolveUpload({
            ok: false,
            cancelled: true,
            message: t('files.cancelled'),
          })
          const job = downloadSection.current
          if (job && message.id && job.id === message.id) {
            finishDownloadRef.current(job, t('files.cancelled'), true)
          }
          break
        }
        case 'cancel':
          // A host-side write failure on an upload (out-of-order chunk,
          // disk error) arrives as `cancel`; it is not a user cancellation.
          resolveUpload({
            ok: false,
            message: message.message ?? t('files.error'),
          })
          break
        case 'error': {
          const consumed = resolveUpload({
            ok: false,
            code: message.code,
            message: message.message ?? t('files.error'),
          })
          if (!consumed) setError(message.message ?? t('files.error'))
          break
        }
        case 'clipboard':
          if (message.ok) {
            setClipboardNote(
              t('files.copied', { count: message.count ?? 0 }),
            )
            setError(null)
          } else {
            setError(message.message ?? t('files.copyFailed'))
          }
          break
        default:
          break
      }
    },
    [
      armDownloadTimeout,
      finishBrowse,
      patchChild,
      resolveUpload,
      t,
    ],
  )

  const handleChunk = useCallback(
    (id: number, offset: number, data: Uint8Array) => {
      const job = downloadSection.current
      if (!job || job.id !== id) return
      if (!acceptDownloadChunk(job, offset, data.byteLength)) {
        finishDownloadRef.current(job, t('files.incomplete'))
        return
      }
      if (job.fsTemp) {
        if (token === '') {
          finishDownloadRef.current(job, t('files.saveFailed'))
          return
        }
        // Copy the socket buffer; it is reused after this handler returns.
        const chunk = new Uint8Array(data)
        const truncate = offset === 0
        job.writeChain = job.writeChain.then(async () => {
          if (job.cancelled) return
          try {
            await writeLocalFileChunk(
              token,
              job.fsTemp!,
              offset,
              chunk,
              truncate,
            )
          } catch (writeError) {
            job.writeError = writeError
          }
        })
      } else if (job.writable) {
        // Copy into a plain ArrayBuffer: the socket buffer may be reused, and
        // FileSystemWritableFileStream rejects a SharedArrayBuffer-backed view.
        const chunk = new Uint8Array(data).buffer
        job.writeChain = job.writeChain.then(async () => {
          if (job.cancelled) return
          try {
            await job.writable!.write(chunk)
          } catch (writeError) {
            job.writeError = writeError
          }
        })
      } else {
        job.chunks.push(data)
      }
      armDownloadTimeout()
      if (job.taskKey !== null && job.childKey) {
        patchProgress(job.taskKey, job.childKey, job.received)
      }
      setDownload({
        key: job.key,
        phase: 'transferring',
        name: job.name,
        received: job.received,
        size: job.size,
      })
      void job.writeChain.then(() => {
        if (job.cancelled || job.finishing) return
        if (job.writeError !== undefined) {
          finishDownloadRef.current(job, t('files.saveFailed'))
          return
        }
        if (downloadComplete(job)) {
          finishDownloadRef.current(job)
          return
        }
        if (job.eofSeen) {
          finishDownloadRef.current(job, t('files.incomplete'))
          return
        }
        if (job.received < job.size) {
          connectionRef.current?.sendFileAction('pull', { id: job.id })
        }
      })
    },
    [armDownloadTimeout, patchProgress, t, token],
  )

  const messageRef = useRef(handleMessage)
  const chunkRef = useRef(handleChunk)
  useEffect(() => {
    messageRef.current = handleMessage
    chunkRef.current = handleChunk
  })

  useEffect(() => {
    if (!connection) return
    return connection.addListener((event) => {
      if (event.kind === 'clipboard-files') {
        setClipboardFiles(event.files)
        return
      }
      if (event.kind === 'file') {
        messageRef.current(event.message)
        return
      }
      if (event.kind === 'file-chunk') {
        chunkRef.current(event.id, event.offset, event.data)
      }
    })
  }, [connection])

  useEffect(() => {
    if (!open || !connection) return
    connection.requestClipboardFiles()
  }, [connection, open])

  // ------------------------------------------------------- local navigation
  const localLabel = source?.kind === 'handle'
    ? [source.rootName, ...handleLevels.map((level) => level.name)].join('/')
    : source?.kind === 'fs'
      ? source.path
      : source?.kind === 'snapshot'
        ? snapshotPath || source.rootName || source.label
        : ''

  const rememberLocal = useCallback((label: string) => {
    setLocalInput(label)
    if (label !== '') storePath(LOCAL_PATH_KEY, label)
  }, [])

  const loadHandleDirectory = useCallback(
    async (
      rootName: string,
      dir: FileSystemDirectoryHandle,
      levels: HandleLevel[],
    ) => {
      const token = ++localLoadToken.current
      setLocalLoading(true)
      try {
        const entries = await readDirectory(dir)
        if (token !== localLoadToken.current) return
        setHandleEntries(entries)
        setHandleLevels(levels)
        setLocalSelection(new Set())
        const label = [rootName, ...levels.map((level) => level.name)].join('/')
        rememberLocal(label)
        setError(null)
        setLocalNote(t('files.handleHint'))
      } catch (loadError) {
        if (token === localLoadToken.current) {
          setError(errorText(loadError) || t('files.error'))
        }
      } finally {
        if (token === localLoadToken.current) setLocalLoading(false)
      }
    },
    [rememberLocal, t],
  )

  /// List a folder on this machine through the local core. Only usable when
  /// the page itself is served by the local core (desktop client): a remote
  /// browser must not enumerate the host it is controlling.
  ///
  /// `remember` is false for the default home the panel opens on by itself:
  /// writing that into LOCAL_PATH_KEY would look like a path the user picked
  /// and could overwrite a real choice on the next launch.
  const loadLocalDirectory = useCallback(
    async (path: string, remember = true) => {
      if (!localMode || token === '') {
        setError(t('files.localPathNeedsClient'))
        return
      }
      const loadToken = ++localLoadToken.current
      setLocalLoading(true)
      try {
        // An empty request means the home folder here; the volume list has
        // its own `roots=1` route.
        const result = await fetchLocalDirectory(
          token,
          path.trim() === '' ? '~' : path,
        )
        if (loadToken !== localLoadToken.current) return
        setSource({
          kind: 'fs',
          path: result.path,
          parent: result.parent,
          root: result.root,
          roots: result.roots ?? [],
        })
        setFsEntries(result.entries)
        setHandleEntries([])
        setHandleLevels([])
        setSnapshotPath('')
        setLocalSelection(new Set())
        setLocalInput(result.path)
        if (remember) storePath(LOCAL_PATH_KEY, result.path)
        setLocalNote(t('files.fsListHint'))
        setError(null)
      } catch {
        if (loadToken === localLoadToken.current) {
          setError(t('files.localPathMissing'))
        }
      } finally {
        if (loadToken === localLoadToken.current) setLocalLoading(false)
      }
    },
    [localMode, token, t],
  )

  // In the desktop shell the local pane starts on the folder last used, or on
  // this machine's home (`/api/fs/list` resolves an empty path to $HOME) when
  // nothing was remembered. The default home is deliberately not written into
  // LOCAL_PATH_KEY: only a path the user actually opened should be
  // remembered. A browser-only page cannot read an arbitrary local path, so
  // it explains how to pick a folder instead.
  useEffect(() => {
    if (!open) {
      localInitial.current = null
      return
    }
    if (!localMode || token === '') return
    const stored = readStored(LOCAL_PATH_KEY)
    const remembered = stored.trim() !== ''
    const key = `${token}:${stored}`
    if (localInitial.current === key) return
    localInitial.current = key
    void loadLocalDirectory(remembered ? stored : '', remembered)
  }, [loadLocalDirectory, localMode, open, token])

  const chooseLocalDirectory = useCallback(() => {
    setError(null)
    const mode = directoryPickMode({
      secureContext: window.isSecureContext === true,
      showDirectoryPicker: window.showDirectoryPicker,
      shellBridge: shellDirectoryPickerCapable(),
      supportsDirectoryInput: supportsDirectoryInput(document),
    })
    if (mode === 'shell') {
      // The GTK shell opens a native SELECT_FOLDER dialog and sends the
      // absolute path back as `molink-directory` (handled below). Cancelling
      // dispatches nothing, so there is no stale/empty path to apply.
      if (requestShellDirectoryPick()) {
        setLocalNote(t('files.dirPickerShell'))
      }
      return
    }
    if (mode === 'directory-input') {
      const input = directoryInputRef.current
      // Chromium on an insecure origin cannot use showDirectoryPicker but
      // still implements `webkitdirectory`; that is a real folder chooser.
      setLocalNote(t('files.dirPickerInputFallback'))
      input?.click()
      return
    }
    if (mode === 'unsupported') {
      // Never click the hidden file input here: a browser that ignores
      // `webkitdirectory` turns it into a plain file dialog, which is the
      // exact "选择目录变成选文件" bug. Say what to do instead.
      setLocalNote(t('files.dirPickerUnsupported'))
      return
    }
    void (async () => {
      try {
        const dir = await window.showDirectoryPicker!({ mode: 'readwrite' })
        setSource({ kind: 'handle', root: dir, rootName: dir.name })
        setSnapshotPath('')
        setFsEntries([])
        setHandleEntries([])
        setHandleLevels([])
        setLocalNote(null)
        await loadHandleDirectory(dir.name, dir, [])
      } catch (pickError) {
        if (!isPickerCancellation(pickError)) {
          setError(errorText(pickError) || t('files.error'))
        }
      }
    })()
  }, [loadHandleDirectory, t])

  // The shell's native folder chooser reports the absolute path here.
  useEffect(() => {
    if (!open) return
    return onShellDirectoryPicked((path) => {
      void loadLocalDirectory(path)
    })
  }, [loadLocalDirectory, open])

  const applySnapshot = useCallback(
    (
      rootName: string,
      files: LocalFile[],
      dirs: string[],
      label: string,
      startPath: string,
    ) => {
      localLoadToken.current += 1
      setSource({
        kind: 'snapshot',
        rootName,
        basePath: startPath,
        files,
        dirs,
        label,
      })
      setHandleEntries([])
      setHandleLevels([])
      setFsEntries([])
      setLocalLoading(false)
      setSnapshotPath(startPath)
      setLocalSelection(new Set())
      setLocalInput(startPath || label)
      if (startPath !== '') storePath(LOCAL_PATH_KEY, startPath)
      setLocalNote(t('files.snapshotHint'))
      setError(null)
    },
    [t],
  )

  const applyInputFiles = useCallback(
    (files: FileList | null, fromDirectory: boolean) => {
      if (!files || files.length === 0) return
      if (fromDirectory) {
        const picked = Array.from(files)
        // A browser that does not implement `webkitdirectory` treats the same
        // control as a plain file picker. `webkitRelativePath` is the reliable
        // signal; without it the flat names are all we have, so explain that
        // the folder structure was not available.
        if (picked.every((file) => file.webkitRelativePath === '')) {
          applySnapshot(
            '',
            picked.map((file) => ({
              path: file.name,
              size: file.size,
              file,
            })),
            [],
            t('files.pickedFiles'),
            '',
          )
          setLocalNote(t('files.dirPickerUnsupported'))
          return
        }
        const built = sourceFromInputFiles(
          picked.map((file) => ({
            file,
            path: file.webkitRelativePath || file.name,
          })),
        )
        applySnapshot(
          built.rootName,
          built.files.map((item) => ({
            path: item.path,
            size: item.size,
            file: item.file,
          })),
          built.dirs,
          built.rootName || t('files.pickedFiles'),
          built.rootName,
        )
        return
      }
      applySnapshot(
        '',
        Array.from(files).map((file) => ({
          path: file.name,
          size: file.size,
          file,
        })),
        [],
        t('files.pickedFiles'),
        '',
      )
    },
    [applySnapshot, t],
  )

  const applyDropped = useCallback(
    (staged: StagedFile[]) => {
      if (staged.length === 0) {
        setError(t('files.emptyDrop'))
        return
      }
      const built = sourceFromInputFiles(
        staged.map((item) => ({
          file: item.file,
          path: item.path === '' ? item.file.name : item.path,
        })),
      )
      applySnapshot(
        built.rootName,
        built.files.map((item) => ({
          path: item.path,
          size: item.size,
          file: item.file,
        })),
        built.dirs,
        t('files.dropped'),
        '',
      )
    },
    [applySnapshot, t],
  )

  useEffect(() => onDroppedFiles(applyDropped), [applyDropped])

  const localRows = useMemo<LocalRow[]>(() => {
    if (source?.kind === 'handle') {
      return handleEntries.map((entry) => ({
        key: entry.name,
        name: entry.name,
        rel: entry.name,
        dir: entry.dir,
        size: entry.size,
        handle: entry.handle,
        file: null,
      }))
    }
    if (source?.kind === 'fs') {
      return fsEntries.map((entry) => ({
        key: entry.name,
        name: entry.name,
        rel: entry.name,
        dir: entry.dir,
        size: entry.size,
        handle: null,
        file: null,
      }))
    }
    if (source?.kind === 'snapshot') {
      const fileByPath = new Map(
        source.files.map((file) => [file.path, file]),
      )
      return localEntriesAt(source.files, source.dirs, snapshotPath).map(
        (entry) => {
          const rel = snapshotPath === ''
            ? entry.path
            : entry.path.startsWith(snapshotPath + '/')
              ? entry.path.slice(snapshotPath.length + 1)
              : entry.name
          return {
            key: rel,
            name: entry.name,
            rel,
            dir: entry.dir,
            size: entry.size,
            handle: null,
            file: entry.dir ? null : fileByPath.get(entry.path)?.file ?? null,
          }
        },
      )
    }
    return []
  }, [fsEntries, handleEntries, snapshotPath, source])

  const currentHandle = source?.kind === 'handle'
    ? handleLevels.length > 0
      ? handleLevels[handleLevels.length - 1]!.handle
      : source.root
    : null

  const navigateLocalDirectory = useCallback(
    (row: LocalRow) => {
      if (!row.dir || !source) return
      if (source.kind === 'handle') {
        if (!row.handle || row.handle.kind !== 'directory') return
        const directory = row.handle as FileSystemDirectoryHandle
        const levels = [
          ...handleLevels,
          { name: row.name, handle: directory },
        ]
        void loadHandleDirectory(source.rootName, directory, levels)
        return
      }
      if (source.kind === 'fs') {
        void loadLocalDirectory(joinPath(source.path, row.name))
        return
      }
      setSnapshotPath(
        snapshotPath === '' ? row.rel : `${snapshotPath}/${row.rel}`,
      )
      setLocalSelection(new Set())
    },
    [
      handleLevels,
      loadHandleDirectory,
      loadLocalDirectory,
      snapshotPath,
      source,
    ],
  )

  const localUp = useCallback(() => {
    if (!source) return
    if (source.kind === 'handle') {
      if (handleLevels.length === 0) return
      const levels = handleLevels.slice(0, -1)
      const dir = levels.length > 0
        ? levels[levels.length - 1]!.handle
        : source.root
      void loadHandleDirectory(source.rootName, dir, levels)
      return
    }
    if (source.kind === 'fs') {
      if (source.parent === '' || source.parent === source.path) return
      void loadLocalDirectory(source.parent)
      return
    }
    if (snapshotPath === source.basePath) return
    const cut = snapshotPath.lastIndexOf('/')
    const parent = cut >= 0 ? snapshotPath.slice(0, cut) : source.basePath
    const target = parent.length < source.basePath.length
      ? source.basePath
      : parent
    setSnapshotPath(target)
    rememberLocal(target || source.label)
    setLocalSelection(new Set())
  }, [
    handleLevels,
    loadHandleDirectory,
    loadLocalDirectory,
    rememberLocal,
    snapshotPath,
    source,
  ])

  const localHome = useCallback(() => {
    if (!source) return
    if (source.kind === 'handle') {
      setHandleLevels([])
      void loadHandleDirectory(source.rootName, source.root, [])
      return
    }
    if (source.kind === 'fs') {
      void loadLocalDirectory(source.root)
      return
    }
    setSnapshotPath(source.basePath)
    rememberLocal(source.basePath || source.label)
    setLocalSelection(new Set())
  }, [loadHandleDirectory, loadLocalDirectory, rememberLocal, source])

  const refreshLocal = useCallback(() => {
    if (!source) return
    if (source.kind === 'handle' && currentHandle) {
      void loadHandleDirectory(source.rootName, currentHandle, handleLevels)
    } else if (source.kind === 'fs') {
      void loadLocalDirectory(source.path)
    } else {
      setLocalNote(t('files.snapshotHint'))
    }
  }, [
    currentHandle,
    handleLevels,
    loadHandleDirectory,
    loadLocalDirectory,
    source,
    t,
  ])

  const openLocalPath = useCallback(() => {
    setError(null)
    const text = localInput.trim()
    const canListLocal = localMode && token !== ''
    // A path typed from scratch, or an absolute path, cannot be resolved by
    // the browser's File System Access handle. The local core can list it;
    // a page served by a remote host cannot, and must say so instead of
    // silently doing nothing.
    if (!source && text === '') {
      if (canListLocal) {
        void loadLocalDirectory('')
      } else {
        chooseLocalDirectory()
      }
      return
    }
    if (!source || source.kind === 'fs' || looksAbsolutePath(text)) {
      if (canListLocal) {
        void loadLocalDirectory(
          source?.kind === 'fs' && text === '' ? source.path : text,
        )
      } else {
        setError(t('files.localPathNeedsClient'))
      }
      return
    }
    if (source.kind === 'handle') {
      const normalized = text.replace(/\\/g, '/').replace(/\/+$/, '')
      const rootName = source.rootName
      let rest = normalized
      if (normalized === rootName) rest = ''
      else if (normalized.startsWith(rootName + '/')) {
        rest = normalized.slice(rootName.length + 1)
      } else if (normalized !== '') {
        if (canListLocal) {
          void loadLocalDirectory(text)
        } else {
          setError(t('files.localUnsupportedPath'))
        }
        return
      }
      if (rest === '') {
        setHandleLevels([])
        void loadHandleDirectory(rootName, source.root, [])
        return
      }
      void (async () => {
        let dir = source.root
        const levels: HandleLevel[] = []
        for (const segment of rest.split('/')) {
          if (segment === '' || segment === '.' || segment === '..') {
            if (canListLocal) {
              void loadLocalDirectory(text)
            } else {
              setError(t('files.localPathMissing'))
            }
            return
          }
          try {
            const next = await dir.getDirectoryHandle(segment)
            levels.push({ name: segment, handle: next })
            dir = next
          } catch {
            if (canListLocal) {
              void loadLocalDirectory(text)
            } else {
              setError(t('files.localPathMissing'))
            }
            return
          }
        }
        await loadHandleDirectory(rootName, dir, levels)
      })()
      return
    }

    let target = normalizeRelativePath(text)
    const rootName = source.rootName
    const base = source.basePath
    if (target === '') {
      target = base
    } else if (target !== base && !source.dirs.includes(target)) {
      // A path typed relative to a picked folder: "2024" below "photos".
      const prefixed = rootName === '' ? target : `${rootName}/${target}`
      if (source.dirs.includes(prefixed)) target = prefixed
    }
    const withinBase = base === '' || target === base ||
      target.startsWith(base + '/')
    const known = target === base || source.dirs.includes(target)
    if (!known || !withinBase) {
      if (canListLocal) {
        void loadLocalDirectory(text)
      } else {
        setError(t('files.localPathMissing'))
      }
      return
    }
    setSnapshotPath(target)
    setLocalNote(t('files.snapshotHint'))
    rememberLocal(target || source.label)
    setLocalSelection(new Set())
  }, [
    chooseLocalDirectory,
    loadHandleDirectory,
    loadLocalDirectory,
    localInput,
    localMode,
    rememberLocal,
    source,
    t,
    token,
  ])

  const toggleLocal = useCallback((key: string) => {
    setLocalSelection((current) => {
      const next = new Set(current)
      if (next.has(key)) next.delete(key)
      else next.add(key)
      return next
    })
  }, [])

  const toggleAllLocal = useCallback(() => {
    setLocalSelection((current) => {
      if (current.size === localRows.length) return new Set()
      return new Set(localRows.map((row) => row.key))
    })
  }, [localRows])

  // ------------------------------------------------------ remote navigation
  const browseInitial = useRef<SessionConnection | null>(null)
  useEffect(() => {
    if (!open) {
      browseInitial.current = null
      return
    }
    if (!connection || browseInitial.current === connection) return
    browseInitial.current = connection
    const stored = readStored(REMOTE_PATH_KEY)
    setRemoteInput(stored)
    setLoading(true)
    void requestListing(stored, true).then(
      (result) => setRemoteInput(result.path),
      () => {
        if (stored !== '') {
          setLoading(true)
          void requestListing('', true).then(
            (result) => setRemoteInput(result.path),
            () => undefined,
          )
        }
      },
    )
  }, [connection, open, requestListing])

  const remoteUp = useCallback(() => {
    if (!listing || listing.path === '') return
    // The host reports an empty parent for a volume/share root; that is the
    // drive list, not the user's home.
    browseRemote(listing.parent)
  }, [browseRemote, listing])

  const remoteHome = useCallback(() => {
    browseRemote(listing?.root ?? '')
  }, [browseRemote, listing])

  const remoteCrumbs = useMemo(
    () => (listing ? pathCrumbs(listing.path) : []),
    [listing],
  )

  const remoteActiveRoot = useMemo(() => {
    if (!listing || listing.path === '') return ''
    for (const root of listing.roots) {
      const base = root.path.replace(/[\\/]+$/, '')
      if (listing.path === root.path || listing.path === base ||
          listing.path.startsWith(base + '\\') ||
          listing.path.startsWith(base + '/')) {
        return root.path
      }
    }
    return ''
  }, [listing])

  const localCrumbs = useMemo(
    () => (source?.kind === 'fs' ? pathCrumbs(source.path) : []),
    [source],
  )

  const localActiveRoot = useMemo(() => {
    if (source?.kind !== 'fs') return ''
    for (const root of source.roots) {
      const base = root.path.replace(/[\\/]+$/, '')
      if (source.path === root.path || source.path === base ||
          source.path.startsWith(base + '\\') ||
          source.path.startsWith(base + '/')) {
        return root.path
      }
    }
    return ''
  }, [source])

  /// Make the local pane follow the remote folder. A Windows remote path on a
  /// POSIX local core (or the reverse) cannot exist; confirm before trying.
  const syncRemoteToLocal = useCallback(() => {
    if (!listing || listing.path === '') {
      setError(t('files.needRemoteListing'))
      return
    }
    if (!localMode || token === '') {
      setError(t('files.localPathNeedsClient'))
      return
    }
    const localPath =
      source?.kind === 'fs' ? source.path : readStored(LOCAL_PATH_KEY)
    const remoteFlavor = pathFlavor(listing.path)
    const localFlavor = pathFlavor(localPath)
    if (remoteFlavor !== 'unknown' && localFlavor !== 'unknown' &&
        remoteFlavor !== localFlavor &&
        !window.confirm(t('files.syncMismatch'))) {
      return
    }
    void loadLocalDirectory(listing.path)
  }, [listing, loadLocalDirectory, localMode, source, t, token])

  /// Make the remote pane follow the local folder. Only a folder listed by the
  /// local core has an absolute path to send; a picked/dropped snapshot does
  /// not.
  const syncLocalToRemote = useCallback(() => {
    if (source?.kind !== 'fs') {
      setError(t('files.syncNeedsLocalFs'))
      return
    }
    if (!listing) {
      setError(t('files.needRemoteListing'))
      return
    }
    const localFlavor = pathFlavor(source.path)
    const remoteFlavor = pathFlavor(listing.path)
    if (localFlavor !== 'unknown' && remoteFlavor !== 'unknown' &&
        localFlavor !== remoteFlavor &&
        !window.confirm(t('files.syncMismatch'))) {
      return
    }
    browseRemote(source.path)
  }, [browseRemote, listing, source, t])

  const toggleRemote = useCallback((entry: FileEntry) => {
    setRemoteSelection((current) => {
      const next = new Map(current)
      if (next.has(entry.name)) next.delete(entry.name)
      else next.set(entry.name, entry)
      return next
    })
  }, [])

  const toggleAllRemote = useCallback(() => {
    if (!listing || listing.path === '') return
    setRemoteSelection((current) =>
      current.size === listing.entries.length
        ? new Map()
        : new Map(listing.entries.map((entry) => [entry.name, entry])),
    )
  }, [listing])

  const remoteSelectedEntries = useMemo(() => {
    if (!listing) return [] as Array<FileEntry & { path: string }>
    return [...remoteSelection.values()].map((entry) => ({
      ...entry,
      path: entry.path ?? joinPath(listing.path, entry.name),
    }))
  }, [listing, remoteSelection])

  // ------------------------------------------------------------- upload run
  /// Runs one upload plan against a destination folder. Shared by the
  /// panel's "send over" button and by files dropped on the session picture,
  /// which skip the local pane and go straight to the remote folder.
  const runUploadPlan = useCallback(
    async ({
      plan,
      dest,
      taskName,
      walkTruncated = false,
    }: {
      plan: UploadPlan
      dest: string
      taskName: string
      walkTruncated?: boolean
    }) => {
      setUploadBusy(true)
      if (plan.files.length === 0 && plan.dirs.length === 0) {
        setError(t('files.nothingToSend'))
        setUploadBusy(false)
        return
      }

      const children: TaskChild[] = plan.files.map((file) => ({
        key: `c${childSeq.current++}`,
        name: file.path,
        path: file.path,
        size: file.size,
        progress: 0,
        status: 'queued',
      }))
      const taskKey = createTask('up', taskName, children, {
        skipped: plan.skipped,
        dirsTotal: plan.dirs.length,
      })
      if (walkTruncated) appendNote(taskKey, t('files.walkTruncated'))
      const run: UploadRun = { taskKey, cancelled: false, activeId: 0 }
      uploadRuns.current.set(taskKey, run)
      updateTask(taskKey, (task) => ({ ...task, phase: 'running' }))

      try {
        let unsupportedDirs = false
        let unsupportedDest = false

        for (const dir of plan.dirs) {
          if (run.cancelled) break
          if (unsupportedDirs) {
            updateTask(taskKey, (task) => ({
              ...task,
              dirsFailed: task.dirsFailed + 1,
            }))
            continue
          }
          const answerPromise = waitUpload('mkdir', uploadReplyTimeoutMs)
          connectionRef.current?.sendFileAction('mkdir', { path: dir, dest })
          const answer = await answerPromise
          if (run.cancelled) break
          if (answer.ok) {
            updateTask(taskKey, (task) => ({
              ...task,
              dirsDone: task.dirsDone + 1,
            }))
          } else if (answer.cancelled) {
            break
          } else {
            updateTask(taskKey, (task) => ({
              ...task,
              dirsFailed: task.dirsFailed + 1,
            }))
            if (isUnsupportedAction(answer.code, answer.message)) {
              unsupportedDirs = true
              appendNote(taskKey, t('files.unsupportedDirs'))
            }
          }
        }

        for (const child of children) {
          const file = plan.files.find((item) => item.path === child.path)
          if (run.cancelled) {
            patchChild(taskKey, child.key, { status: 'cancelled' })
            continue
          }
          if (!file) {
            patchChild(taskKey, child.key, {
              status: 'error',
              error: t('files.error'),
            })
            continue
          }
          patchChild(taskKey, child.key, { status: 'running' })

          // A resumed upload names the same `.part` on the host (the key is
          // stable across reconnects) and tells it how much we already sent.
          const resumeKey = uploadResumeKey(
            connectionRef.current?.targetKey ?? '',
            dest,
            file.path,
            file.size,
          )
          const resumeFrom = loadUploadOffset(resumeStore, resumeKey, file.size)
          const answerPromise = waitUpload('offer', uploadReplyTimeoutMs)
          connectionRef.current?.sendFileAction('offer', {
            name: file.path,
            size: file.size,
            dest,
            ...(resumeFrom > 0
              ? { offset: resumeFrom, resume: resumeKey }
              : {}),
          })
          const answer = await answerPromise
          if (run.cancelled || answer.cancelled) {
            if (answer.id) {
              connectionRef.current?.sendFileAction('cancel', {
                id: answer.id,
              })
            }
            patchChild(taskKey, child.key, { status: 'cancelled' })
            continue
          }
          if (!answer.ok || !answer.id) {
            patchChild(taskKey, child.key, {
              status: 'error',
              error: answer.message ?? t('files.error'),
            })
            // A read-only or missing remote folder must say so instead of
            // leaving the failure buried in the task row.
            setError(answer.message ?? t('files.error'))
            continue
          }
          const echoed = answer.dir === undefined
            ? null
            : relativeUnder(dest, answer.dir)
          if (!unsupportedDest && echoed !== '') {
            unsupportedDest = true
            appendNote(taskKey, t('files.unsupportedDest'))
          }

          const id = answer.id
          run.activeId = id
          uploadAckKeys.set(id, { key: resumeKey, size: file.size })
          const chunk = answer.chunk && answer.chunk > 0
            ? answer.chunk
            : 256 * 1024
          // The host is authoritative: it says how much of the `.part` it
          // really has, which may be less than what we offered.
          let offset = Math.min(answer.offset ?? 0, file.size)
          let failed = false
          while (offset < file.size) {
            if (run.cancelled) {
              connectionRef.current?.sendFileAction('cancel', { id })
              patchChild(taskKey, child.key, { status: 'cancelled' })
              failed = true
              break
            }
            let target = connectionRef.current
            if (!target) {
              // The host may restart mid-transfer. Wait for the session to
              // come back instead of throwing the upload away; the resume
              // offset makes the wait cheap even for a gigabyte.
              const live = await waitUntilLive(
                () => connectionRef.current !== null,
                uploadReconnectMs,
                () => run.cancelled,
              )
              target = live ? connectionRef.current : null
            }
            if (!target) {
              patchChild(taskKey, child.key, {
                status: 'error',
                error: t('files.error'),
              })
              failed = true
              break
            }
            const end = Math.min(file.size, offset + chunk)
            let bytes: Uint8Array
            try {
              if (file.file) {
                bytes = new Uint8Array(
                  await file.file.slice(offset, end).arrayBuffer(),
                )
              } else if (file.absPath && token !== '') {
                bytes = await readLocalFileRange(
                  token,
                  file.absPath,
                  offset,
                  end - offset,
                )
              } else {
                throw new Error('no local source')
              }
            } catch {
              patchChild(taskKey, child.key, {
                status: 'error',
                error: t('files.localPathMissing'),
              })
              failed = true
              break
            }
            if (run.cancelled) {
              connectionRef.current?.sendFileAction('cancel', { id })
              patchChild(taskKey, child.key, { status: 'cancelled' })
              failed = true
              break
            }
            target.sendFileChunk(id, offset, bytes)
            offset = end
            patchProgress(taskKey, child.key, offset)
            // Credit window: never run more than kUploadWindowBytes ahead of
            // the host's contiguous point. Without this the file channel's
            // byte budget drops chunks on a slow link and the host cancels the
            // whole upload as "out of order".
            const ack = uploadAckFor(id)
            if (offset - ack.confirmed >= kUploadWindowBytes) {
              const reached = await ack.wait(kUploadAckTimeoutMs)
              if (run.cancelled) continue
              if (ack.failed) {
                patchChild(taskKey, child.key, {
                  status: 'error',
                  error: t('files.error'),
                })
                failed = true
                break
              }
              if (!reached) {
                // No ack in time: rewind to the last confirmed byte and let
                // the loop resend. The host treats the replay as idempotent.
                offset = ack.confirmed
              }
            }
            // Yield so a large file does not freeze the UI thread.
            await new Promise((resolve) => window.setTimeout(resolve, 0))
          }
          run.activeId = 0
          uploadAcks.delete(id)
          uploadAckKeys.delete(id)
          if (!failed) clearUploadOffset(resumeStore, resumeKey)
          if (failed) continue

          patchProgress(taskKey, child.key, offset, true)
          const target = connectionRef.current
          if (!target) {
            patchChild(taskKey, child.key, {
              status: 'error',
              error: t('files.error'),
            })
            continue
          }
          target.sendClientLog(
            `upload chunks done id=${id} sent=${offset} size=${file.size}`,
          )
          const donePromise = waitUpload('done', uploadDoneTimeoutMs)
          target.sendFileAction('complete', { id })
          const done = await donePromise
          if (done.ok) {
            // It won the race with the cancel: the host already renamed the
            // file, so report it as done instead of pretending it vanished.
            patchChild(taskKey, child.key, {
              status: 'done',
              progress: file.size,
              size: file.size,
            })
            if (done.path) {
              updateTask(taskKey, (task) => ({
                ...task,
                remotePaths: [...task.remotePaths, done.path!],
              }))
            }
          } else if (done.cancelled) {
            patchChild(taskKey, child.key, { status: 'cancelled' })
          } else if (run.cancelled) {
            patchChild(taskKey, child.key, { status: 'cancelled' })
          } else {
            patchChild(taskKey, child.key, {
              status: 'error',
              error: done.message ?? t('files.error'),
            })
          }
        }
      } finally {
        uploadRuns.current.delete(taskKey)
        updateTask(taskKey, (task) => ({
          ...task,
          children: task.children.map((child) =>
            child.status === 'queued' || child.status === 'running'
              ? {
                  ...child,
                  status: run.cancelled ? 'cancelled' : 'error',
                }
              : child,
          ),
        }))
        finalizeTask(taskKey)
        setUploadBusy(false)
        const currentListing = listingRef.current
        if (currentListing) {
          void requestListing(currentListing.path, true).catch(
            () => undefined,
          )
        }
      }
    },
    [
      appendNote,
      createTask,
      finalizeTask,
      patchChild,
      patchProgress,
      requestListing,
      t,
      token,
      updateTask,
      waitUpload,
    ],
  )

  /// Reads a remote selection into the file/folder tree both drag-out paths
  /// need. Folders are expanded recursively; a failed listing is recorded
  /// instead of silently dropping that subtree.
  const collectRemoteFiles = useCallback(
    async (
      items: RemoteDragItem[],
      stopped: () => boolean,
    ): Promise<{
      files: RemotePlanFile[]
      dirs: string[]
      failures: string[]
    }> => {
      const files: RemotePlanFile[] = []
      const dirs: string[] = []
      const failures: string[] = []
      const walk = async (path: string, rel: string): Promise<void> => {
        if (stopped()) return
        dirs.push(rel)
        let result: RemoteListing
        try {
          result = await requestListing(path, false)
        } catch (walkError) {
          failures.push(
            `${rel}: ${errorText(walkError) || t('files.error')}`,
          )
          return
        }
        if (stopped()) return
        for (const entry of result.entries) {
          const childPath = joinPath(result.path, entry.name)
          const childRel = rel === '' ? entry.name : `${rel}/${entry.name}`
          if (entry.dir) await walk(childPath, childRel)
          else {
            files.push({ path: childPath, rel: childRel, size: entry.size })
          }
        }
      }
      for (const item of items) {
        if (stopped()) break
        if (item.dir) await walk(item.path, item.name)
        else files.push({ path: item.path, rel: item.name, size: 0 })
      }
      return { files, dirs, failures }
    },
    [requestListing, t],
  )

  /// Sizes of plain files come from the listing that built the selection; the
  /// recursive walk above starts from an entry and only knows names, so fill
  /// in the size for the top-level files from the caller's entries.
  const collectDragOutFiles = useCallback(
    async (items: RemoteDragItem[], stopped: () => boolean) => {
      const walked = await collectRemoteFiles(items, stopped)
      const sizes = new Map(
        items.filter((item) => !item.dir).map((item) => [item.path, item.size]),
      )
      return {
        ...walked,
        files: walked.files.map((file) => ({
          ...file,
          size: sizes.get(file.path) ?? file.size,
        })),
      }
    },
    [collectRemoteFiles],
  )

  // ---------------------------------------------------------- remote drop-in
  const uploadDropped = useCallback(
    async (staged: StagedFile[], dest: string) => {
      const files: LocalFile[] = []
      for (const item of staged) {
        const path = normalizeRelativePath(
          item.path === '' ? item.file.name : item.path,
        )
        if (path === '') continue
        files.push({ path, size: item.file.size, file: item.file })
      }
      const { dirs, selection } = droppedTree(files.map((file) => file.path))
      const plan = buildUploadPlan(files, dirs, selection)
      const taskName = selection.length === 1
        ? selection[0]!.path
        : t('files.itemsCount', { count: selection.length })
      setError(null)
      await runUploadPlan({ plan, dest, taskName })
    },
    [runUploadPlan, t],
  )

  const handleRemoteDrop = useCallback(
    (staged: StagedFile[]) => {
      if (staged.length === 0) {
        setError(t('files.emptyDrop'))
        return
      }
      const dest = dropTarget(
        listingRef.current?.path,
        readStored(REMOTE_PATH_KEY),
      )
      if (dest === '') {
        // No folder is known yet: stage the files in the drawer and ask once.
        applyDropped(staged)
        onRequestOpen?.()
        setError(t('files.dropNeedTarget'))
        return
      }
      // The drawer owns the task list, so opening it is what gives the drop
      // visible per-file progress and the cancel button.
      onRequestOpen?.()
      void uploadDropped(staged, dest)
    },
    [applyDropped, onRequestOpen, t, uploadDropped],
  )

  useEffect(() => onRemoteDropFiles(handleRemoteDrop), [handleRemoteDrop])

  // ----------------------------------------------------------- drag-out
  const [dragOut, setDragOut] = useState<{
    phase: 'preparing' | 'ready' | 'error'
    name: string
    percent: number
    slow: boolean
  } | null>(null)
  const dragOutTicket = useRef(0)
  const dragOutRun = useRef<{ cancelled: boolean } | null>(null)
  const dragOutSlowTimer = useRef(0)
  const dragOutProgressTimer = useRef(0)
  /// A press on a remote row that may become a drag-out. Pointer events are
  /// used instead of an HTML drag: WebKit's own drag keeps the pointer grab,
  /// which made a nested GTK drag unable to reach the desktop.
  const dragOutPointer = useRef<{
    pointerId: number
    items: RemoteDragItem[]
    startX: number
    startY: number
    moved: boolean
    triggered: boolean
  } | null>(null)
  /// Swallow the click WebKit may still deliver after the pointer left for a
  /// drag-out, so releasing on the edge does not also open a folder.
  const dragOutSuppressClick = useRef(false)

  useEffect(() => {
    if (listing) onRemotePathChange?.(listing.path)
    else onRemotePathChange?.(readStored(REMOTE_PATH_KEY))
  }, [listing, onRemotePathChange])

  const localHomePath = useCallback(async (): Promise<string> => {
    if (!localMode || token === '') return ''
    try {
      const home = await fetchLocalDirectory(token, '')
      return (home.root || home.path).trim()
    } catch {
      return ''
    }
  }, [localMode, token])

  /// Prepares local copies under the cache directory, then asks the shell to
  /// start the system drag. The overlay reports progress and can be cancelled.
  const prepareDragOut = useCallback(
    async (items: RemoteDragItem[]) => {
      const ticket = (dragOutTicket.current += 1)
      const run = { cancelled: false }
      dragOutRun.current = run
      const stopped = () =>
        run.cancelled || ticket !== dragOutTicket.current
      const label = items.length === 1
        ? items[0]!.name
        : t('files.itemsCount', { count: items.length })
      setDragOut({
        phase: 'preparing',
        name: label,
        percent: 0,
        slow: false,
      })
      window.clearTimeout(dragOutSlowTimer.current)
      dragOutSlowTimer.current = window.setTimeout(() => {
        setDragOut((current) =>
          current && current.phase === 'preparing'
            ? { ...current, slow: true }
            : current,
        )
      }, 10_000)
      window.clearInterval(dragOutProgressTimer.current)
      const finishOverlay = () => {
        window.clearTimeout(dragOutSlowTimer.current)
        window.clearInterval(dragOutProgressTimer.current)
      }
      let handedOff = false

      try {
        const home = await localHomePath()
        if (stopped()) return
        if (home === '') {
          finishOverlay()
          setDragOut(null)
          setError(t('files.dragOutNeedClient'))
          return
        }
        const id =
          typeof crypto !== 'undefined' &&
          typeof crypto.randomUUID === 'function'
            ? crypto.randomUUID()
            : `${Date.now()}-${Math.random().toString(36).slice(2)}`
        const staging = joinRelative(
          home,
          `.cache/molink/dragout/${id}`,
        )
        await makeLocalDirectory(token, staging)
        if (stopped()) return

        const { files, dirs, failures } = await collectDragOutFiles(
          items,
          stopped,
        )
        if (stopped()) return
        if (files.length === 0 || failures.length > 0) {
          finishOverlay()
          setDragOut(null)
          setError(failures[0] ?? t('files.error'))
          return
        }

        for (const dir of dirs) {
          if (stopped()) return
          const segments = safeTargetSegments(dir, 'folder')
          await makeLocalDirectory(
            token,
            joinRelative(staging, segments.join('/')),
          ).catch(() => undefined)
        }

        const totalBytes = files.reduce((sum, file) => sum + file.size, 0)
        let doneBytes = 0
        const committed: string[] = []
        dragOutProgressTimer.current = window.setInterval(() => {
          const job = downloadSection.current
          const received = job?.received ?? 0
          setDragOut((current) =>
            current && current.phase === 'preparing'
              ? {
                  ...current,
                  percent:
                    totalBytes <= 0
                      ? 0
                      : Math.min(
                          99,
                          Math.round(
                            ((doneBytes + received) / totalBytes) * 100,
                          ),
                        ),
                }
              : current,
          )
        }, 250)

        for (const file of files) {
          if (stopped()) break
          const segments = safeTargetSegments(file.rel, 'file')
          const name = segments[segments.length - 1]!
          const fsTarget = joinRelative(staging, segments.join('/'))
          await new Promise<void>((resolve) => {
            queueJob(
              makeJob({
                path: file.path,
                name,
                taskKey: null,
                childKey: null,
                silent: true,
                strategy: 'button',
                target: null,
                baseDir: null,
                segments: null,
                fsTarget,
                onDone: (path) => {
                  if (path) committed.push(path)
                },
                resolve,
              }),
            )
          })
          doneBytes += file.size
        }

        finishOverlay()
        if (stopped()) {
          setDragOut(null)
          return
        }
        if (committed.length !== files.length) {
          setDragOut(null)
          setError(t('files.dragOutFailed'))
          return
        }
        setDragOut({
          phase: 'ready',
          name: label,
          percent: 100,
          slow: false,
        })
        if (!startShellDragOut({ dir: staging, paths: committed, names: files.map((file) => file.rel) })) {
          setError(t('files.dragOutNeedClient'))
          return
        }
        handedOff = true
        // The native drag runs its own loop; the preparation card has done its
        // job. If the user cancels, the shell keeps the cache for five minutes.
        window.setTimeout(() => setDragOut(null), 1_200)
      } catch (dragError) {
        finishOverlay()
        setDragOut(null)
        setError(errorText(dragError) || t('files.dragOutFailed'))
      } finally {
        if (dragOutRun.current === run) dragOutRun.current = null
        // A newer gesture owns the overlay; only the run that still owns it
        // may tear it down.
        if (!handedOff && ticket === dragOutTicket.current) {
          finishOverlay()
          setDragOut(null)
        }
      }
    },
    [
      collectDragOutFiles,
      localHomePath,
      makeJob,
      queueJob,
      t,
      token,
    ],
  )

  const cancelDragOut = useCallback(() => {
    dragOutTicket.current += 1
    const run = dragOutRun.current
    if (run) run.cancelled = true
    const job = downloadSection.current
    if (job) finishDownloadRef.current(job, t('files.cancelled'), true)
    dragOutRun.current = null
    window.clearTimeout(dragOutSlowTimer.current)
    window.clearInterval(dragOutProgressTimer.current)
    setDragOut(null)
  }, [t])

  /// The browser fallback: fetch the selection and hand it to the browser as a
  /// normal download. The page cannot start a system drag, which is why this
  /// path exists at all.
  const fallbackDragOut = useCallback(
    async (items: RemoteDragItem[]) => {
      setError(t('files.dragOutFallback'))
      const run = { cancelled: false }
      const { files } = await collectDragOutFiles(items, () => run.cancelled)
      if (files.length === 0) {
        setError(t('files.error'))
        return
      }
      const children: TaskChild[] = files.map((file) => ({
        key: `c${childSeq.current++}`,
        name: file.rel,
        path: file.path,
        size: file.size,
        progress: 0,
        status: 'queued',
      }))
      const taskKey = createTask('down', t('files.dragOutDownload'), children)
      const downloadRun: DownloadRun = { taskKey, cancelled: false }
      downloadRuns.current.set(taskKey, downloadRun)
      try {
        for (let index = 0; index < files.length; index += 1) {
          const file = files[index]!
          if (downloadRun.cancelled) break
          const segments = safeTargetSegments(file.rel, 'file')
          const name = segments[segments.length - 1]!
          await new Promise<void>((resolve) => {
            queueJob(
              makeJob({
                path: file.path,
                name,
                taskKey,
                childKey: children[index]!.key,
                silent: true,
                strategy: 'button',
                target: null,
                baseDir: null,
                segments: null,
                autoDownload: true,
                resolve,
              }),
            )
          })
        }
      } finally {
        downloadRuns.current.delete(taskKey)
        finalizeTask(taskKey)
      }
    },
    [collectDragOutFiles, createTask, finalizeTask, makeJob, queueJob, t],
  )

  const triggerDragOut = useCallback(
    (items: RemoteDragItem[]) => {
      if (shellDragOutCapable() && localMode && token !== '') {
        void prepareDragOut(items)
      } else {
        void fallbackDragOut(items)
      }
    },
    [fallbackDragOut, localMode, prepareDragOut, token],
  )

  /// Which remote entries the press would drag: the whole selection when the
  /// row is part of it, otherwise just that row.
  const dragItemsAt = useCallback(
    (entry: FileEntry, entryPath: string): RemoteDragItem[] => {
      if (!listing || listing.path === '') return []
      const selected: RemoteDragItem[] = [...remoteSelection.values()].map(
        (item) => ({
          path: item.path ?? joinPath(listing.path, item.name),
          name: item.name,
          dir: item.dir,
          size: item.size,
        }),
      )
      return dragItemsFor(
        { path: entryPath, name: entry.name, dir: entry.dir, size: entry.size },
        selected,
      )
    },
    [listing, remoteSelection],
  )

  const onRemoteDragStart = useCallback(
    (event: DragEvent, entry: FileEntry, entryPath: string) => {
      const items = dragItemsAt(entry, entryPath)
      // The requirement is that the page never starts a local file drag: it
      // has no bytes yet and the shell owns the real drag. Pointer events keep
      // tracking the held button after this cancellation.
      event.preventDefault()
      if (items.length === 0) return
      const payload = buildRemoteDragPayload(items)
      event.dataTransfer.setData(
        REMOTE_DRAG_MIME,
        encodeRemoteDragPayload(payload),
      )
      event.dataTransfer.setData('text/plain', payload.names.join('\n'))
      event.dataTransfer.effectAllowed = 'copy'
    },
    [dragItemsAt],
  )

  const onRemotePointerDown = useCallback(
    (event: ReactPointerEvent<HTMLLIElement>, entry: FileEntry, entryPath: string) => {
      dragOutSuppressClick.current = false
      if (event.button !== 0) return
      // Checkboxes and the per-row download button keep their own gestures.
      const target = event.target as Element | null
      if (target?.closest('input, a, [data-no-drag]')) return
      const items = dragItemsAt(entry, entryPath)
      if (items.length === 0) return
      dragOutPointer.current = {
        pointerId: event.pointerId,
        items,
        startX: event.clientX,
        startY: event.clientY,
        moved: false,
        triggered: false,
      }
    },
    [dragItemsAt],
  )

  // The pointer still belongs to the page while the button is held, even when
  // it has left the window. Reaching the edge is the signal to prepare the
  // local copies; the native drag starts when they exist.
  useEffect(() => {
    const trigger = (gesture: { items: RemoteDragItem[]; triggered: boolean }) => {
      if (gesture.triggered) return
      gesture.triggered = true
      dragOutSuppressClick.current = true
      triggerDragOut(gesture.items)
    }
    const onMove = (event: globalThis.PointerEvent) => {
      const gesture = dragOutPointer.current
      if (!gesture || event.pointerId !== gesture.pointerId) return
      if (
        !gesture.moved &&
        Math.hypot(
          event.clientX - gesture.startX,
          event.clientY - gesture.startY,
        ) > 6
      ) {
        gesture.moved = true
      }
      if (
        gesture.moved &&
        isOutsideWindow(
          event.clientX,
          event.clientY,
          window.innerWidth,
          window.innerHeight,
        )
      ) {
        trigger(gesture)
      }
    }
    const onUp = (event: globalThis.PointerEvent) => {
      const gesture = dragOutPointer.current
      if (!gesture || event.pointerId !== gesture.pointerId) return
      dragOutPointer.current = null
      if (gesture.moved) dragOutSuppressClick.current = true
    }
    const onCancel = () => {
      dragOutPointer.current = null
    }
    const onLeave = () => {
      const gesture = dragOutPointer.current
      if (!gesture || !gesture.moved) return
      trigger(gesture)
    }
    window.addEventListener('pointermove', onMove)
    window.addEventListener('pointerup', onUp)
    window.addEventListener('pointercancel', onCancel)
    document.documentElement.addEventListener('pointerleave', onLeave)
    return () => {
      window.removeEventListener('pointermove', onMove)
      window.removeEventListener('pointerup', onUp)
      window.removeEventListener('pointercancel', onCancel)
      document.documentElement.removeEventListener('pointerleave', onLeave)
    }
  }, [triggerDragOut])

  const sendOver = useCallback(async () => {
    if (!source || listing === null || localSelection.size === 0) return
    const dest = listing.path
    setError(null)
    setUploadBusy(true)
    try {
      let plan: UploadPlan
      let walkTruncated = false
      if (source.kind === 'snapshot') {
        const base = snapshotPath
        const strip = (path: string) =>
          base === '' ? path : path.slice(base.length + 1)
        const files = source.files
          .filter((file) => base === '' || file.path.startsWith(base + '/'))
          .map((file) => ({ ...file, path: strip(file.path) }))
        const dirs = source.dirs
          .filter((dir) => base === '' || dir.startsWith(base + '/'))
          .map(strip)
        const selection = [...localSelection].map((rel) => {
          const row = localRows.find((item) => item.rel === rel)
          return { path: rel, dir: row?.dir ?? false }
        })
        plan = buildUploadPlan(files, dirs, selection)
      } else if (source.kind === 'fs') {
        // Desktop shell: the local core can read every file under the open
        // folder, so walk the selected folders through `/api/fs/list` and read
        // the bytes lazily while uploading.
        if (!localMode || token === '') {
          setError(t('files.localPathNeedsClient'))
          setUploadBusy(false)
          return
        }
        const files: LocalFile[] = []
        const dirs: string[] = []
        let skipped = 0
        let budget = 20000
        const walkFs = async (
          path: string,
          prefix: string,
          depth: number,
        ): Promise<void> => {
          if (budget <= 0 || depth > 64) {
            walkTruncated = true
            return
          }
          const result = await fetchLocalDirectory(token, path)
          for (const entry of result.entries) {
            if (budget <= 0) {
              walkTruncated = true
              return
            }
            budget -= 1
            const rel = prefix === '' ? entry.name : `${prefix}/${entry.name}`
            if (entry.dir) {
              dirs.push(rel)
              // Never follow a symlink/junction folder: a loop would recurse
              // forever just like it would on the remote host.
              if (!entry.link) {
                await walkFs(joinPath(result.path, entry.name), rel, depth + 1)
              }
            } else {
              files.push({
                path: rel,
                size: entry.size,
                file: null,
                absPath: joinPath(result.path, entry.name),
              })
            }
          }
        }
        for (const row of localRows) {
          if (!localSelection.has(row.rel)) continue
          if (row.dir) {
            dirs.push(row.name)
            try {
              await walkFs(joinPath(source.path, row.name), row.name, 0)
            } catch {
              skipped += 1
            }
          } else {
            files.push({
              path: row.name,
              size: row.size,
              file: null,
              absPath: joinPath(source.path, row.name),
            })
          }
        }
        const selection = [...localSelection].map((rel) => {
          const row = localRows.find((item) => item.rel === rel)
          return { path: rel, dir: row?.dir ?? false }
        })
        plan = buildUploadPlan(files, dirs, selection)
        plan = { ...plan, skipped: plan.skipped + skipped }
      } else {
        const files: LocalFile[] = []
        const dirs: string[] = []
        let skipped = 0
        const walk = async (
          dir: FileSystemDirectoryHandle,
          prefix: string,
        ): Promise<void> => {
          for await (const handle of directoryHandles(dir)) {
            const path = `${prefix}/${handle.name}`
            if (handle.kind === 'directory') {
              dirs.push(path)
              await walk(handle as FileSystemDirectoryHandle, path)
            } else {
              try {
                const file = await (handle as FileSystemFileHandle).getFile()
                files.push({ path, size: file.size, file })
              } catch {
                skipped += 1
              }
            }
          }
        }
        for (const row of localRows) {
          if (!localSelection.has(row.rel)) continue
          if (row.dir) {
            if (!row.handle || row.handle.kind !== 'directory') {
              skipped += 1
              continue
            }
            dirs.push(row.name)
            try {
              await walk(row.handle as FileSystemDirectoryHandle, row.name)
            } catch {
              skipped += 1
            }
          } else if (row.handle && row.handle.kind === 'file') {
            try {
              const file = await (row.handle as FileSystemFileHandle).getFile()
              files.push({ path: row.name, size: file.size, file })
            } catch {
              skipped += 1
            }
          } else {
            skipped += 1
          }
        }
        plan = buildUploadPlan(files, dirs, [...localSelection].map((rel) => {
          const row = localRows.find((item) => item.rel === rel)
          return { path: rel, dir: row?.dir ?? false }
        }))
        plan = { ...plan, skipped: plan.skipped + skipped }
      }

      const selectedRows = localRows.filter((row) =>
        localSelection.has(row.rel))
      const taskName = selectedRows.length === 1
        ? selectedRows[0]!.name
        : t('files.itemsCount', { count: selectedRows.length })
      await runUploadPlan({ plan, dest, taskName, walkTruncated })
    } catch (uploadError) {
      setUploadBusy(false)
      setError(errorText(uploadError) || t('files.error'))
    }
  }, [
    localMode,
    localRows,
    localSelection,
    listing,
    runUploadPlan,
    snapshotPath,
    source,
    t,
    token,
  ])

  // ----------------------------------------------------------- download run
  const receiveBack = useCallback(async () => {
    if (!listing || remoteSelection.size === 0) return
    // A browser-only page needs a directory handle. The desktop shell can
    // write into the local core's current folder instead.
    const fsBase =
      source?.kind === 'fs' && localMode && token !== '' ? source.path : ''
    const destination = currentHandle
    if (!destination && fsBase === '') return
    const remoteRoot = listing.path
    const selected = remoteSelectedEntries
    const taskName = selected.length === 1
      ? selected[0]!.name
      : t('files.itemsCount', { count: selected.length })
    setError(null)
    setBulkBusy(true)
    const taskKey = createTask('down', taskName, [])
    const run: DownloadRun = { taskKey, cancelled: false }
    downloadRuns.current.set(taskKey, run)

    try {
      const files: RemotePlanFile[] = []
      const dirs: string[] = []
      const failures: Array<{ rel: string; error: string }> = []

      const walk = async (path: string, rel: string): Promise<void> => {
        if (run.cancelled) return
        dirs.push(rel)
        let result: RemoteListing
        try {
          result = await requestListing(path, false)
        } catch (walkError) {
          failures.push({
            rel,
            error: errorText(walkError) || t('files.error'),
          })
          return
        }
        if (run.cancelled) return
        for (const entry of result.entries) {
          const childPath = joinPath(result.path, entry.name)
          const childRel = relativeUnder(remoteRoot, childPath) ??
            (rel === '' ? entry.name : `${rel}/${entry.name}`)
          if (entry.dir) await walk(childPath, childRel)
          else {
            files.push({
              path: childPath,
              rel: childRel,
              size: entry.size,
              mtime: entry.mtime,
            })
          }
        }
      }

      for (const entry of selected) {
        if (run.cancelled) break
        if (entry.dir) {
          await walk(entry.path, entry.name)
        } else {
          files.push({
            path: entry.path,
            rel: relativeUnder(remoteRoot, entry.path) ?? entry.name,
            size: entry.size,
            mtime: entry.mtime,
          })
        }
      }

      if (run.cancelled) return

      const children: TaskChild[] = files.map((file) => ({
        key: `c${childSeq.current++}`,
        name: file.rel,
        path: file.path,
        size: file.size,
        progress: 0,
        status: 'queued',
      }))
      const failureChildren: TaskChild[] = failures.map((failure) => ({
        key: `c${childSeq.current++}`,
        name: failure.rel,
        size: 0,
        progress: 0,
        status: 'error',
        error: failure.error,
      }))
      updateTask(taskKey, (task) => ({
        ...task,
        children: [...children, ...failureChildren],
        dirsTotal: dirs.length,
        phase: 'running',
      }))

      for (const dir of dirs) {
        if (run.cancelled) break
        try {
          if (fsBase !== '') {
            await makeLocalDirectory(token, joinRelative(fsBase, dir))
          } else if (destination) {
            const segments = safeTargetSegments(dir, 'folder')
            await ensureDirectory(destination, segments)
          }
          updateTask(taskKey, (task) => ({
            ...task,
            dirsDone: task.dirsDone + 1,
          }))
        } catch {
          updateTask(taskKey, (task) => ({
            ...task,
            dirsFailed: task.dirsFailed + 1,
          }))
        }
      }

      for (const child of children) {
        if (run.cancelled) {
          patchChild(taskKey, child.key, { status: 'cancelled' })
          continue
        }
        const file = files.find((item) => item.rel === child.name)
        if (!file) {
          patchChild(taskKey, child.key, {
            status: 'error',
            error: t('files.error'),
          })
          continue
        }
        const segments = safeTargetSegments(file.rel, 'file')
        const name = segments[segments.length - 1]!
        const parent = segments.slice(0, -1)
        const fsTarget = fsBase === ''
          ? null
          : joinRelative(fsBase, segments.join('/'))
        await new Promise<void>((resolve) => {
          const job = makeJob({
            path: file.path,
            name,
            taskKey,
            childKey: child.key,
            silent: true,
            strategy: fsTarget ? 'button' : 'picker',
            target: null,
            baseDir: fsTarget ? null : destination,
            segments: fsTarget ? null : parent,
            fsTarget,
            mtime: file.mtime ?? 0,
            resolve,
          })
          queueJob(job)
        })
      }
    } catch (receiveError) {
      setError(errorText(receiveError) || t('files.error'))
    } finally {
      downloadRuns.current.delete(taskKey)
      updateTask(taskKey, (task) => ({
        ...task,
        children: task.children.map((child) =>
          child.status === 'queued' || child.status === 'running'
            ? { ...child, status: run.cancelled ? 'cancelled' : 'error' }
            : child,
        ),
      }))
      finalizeTask(taskKey)
      setBulkBusy(false)
    }
  }, [
    createTask,
    currentHandle,
    finalizeTask,
    listing,
    localMode,
    makeJob,
    patchChild,
    queueJob,
    remoteSelectedEntries,
    remoteSelection.size,
    requestListing,
    source,
    t,
    token,
    updateTask,
  ])

  // ------------------------------------------------------ connection cleanup
  // The unsubscribe below must not run on a language change, so the message
  // is read through a ref instead of putting `t` in the dependency list.
  const cancelledMessage = useRef(t('files.cancelled'))
  useEffect(() => {
    cancelledMessage.current = t('files.cancelled')
  })

  useEffect(() => {
    return () => {
      const message = cancelledMessage.current
      const active = downloadSection.current
      if (active) {
        active.cancelled = true
        window.clearTimeout(active.timer)
        void discardPartial(active)
        if (active.id !== 0) {
          connection?.sendFileAction('cancel', { id: active.id })
        }
        active.resolve()
      }
      for (const run of downloadRuns.current.values()) run.cancelled = true
      for (const queued of downloadQueue.current) {
        // Job records are plain objects owned by this panel, not React state;
        // the pump reads the flag off the same object.
        // eslint-disable-next-line react-hooks/immutability
        queued.cancelled = true
        if (queued.target) {
          void queued.target
            .then((writable) => writable.abort())
            .catch(() => undefined)
        }
        queued.resolve()
      }
      downloadSection.current = null
      downloadQueue.current = []
      for (const run of uploadRuns.current.values()) {
        run.cancelled = true
        if (run.activeId !== 0) {
          connection?.sendFileAction('cancel', { id: run.activeId })
        }
      }
      const waiter = uploadWaiter.current
      if (waiter) {
        window.clearTimeout(waiter.timer)
        uploadWaiter.current = null
        waiter.resolve({
          ok: false,
          cancelled: true,
          message,
        })
      }
      const activeBrowse = browseActive.current
      if (activeBrowse) {
        window.clearTimeout(activeBrowse.timer)
        browseActive.current = null
        activeBrowse.reject(new Error(message))
      }
      for (const request of browseQueue.current) {
        request.reject(new Error(message))
      }
      browseQueue.current = []
      setLoading(false)
    }
  }, [connection, discardPartial])

  const saveReadyFile = (item: ReadySave) => {
    // Runs from the button's click: the download is user-confirmed even
    // though the bytes arrived earlier.
    const url = URL.createObjectURL(item.blob)
    const anchor = document.createElement('a')
    anchor.href = url
    anchor.download = suggestedFileName(item.name, item.name)
    document.body.appendChild(anchor)
    anchor.click()
    anchor.remove()
    window.setTimeout(() => URL.revokeObjectURL(url), 30_000)
    setSaveReady((current) => current.filter((entry) => entry.key !== item.key))
  }

  if (!open) return null

  const directionMode: DownloadMode = downloadModeFor(window)
  /// A browser-only page cannot read this machine's home folder through
  /// /api/fs/list, so it starts with the picker explanation instead of a
  /// silent empty pane. A user action's note always wins.
  const displayedLocalNote =
    localNote ??
    (!localMode && source === null ? t('files.defaultHomeBrowser') : null)
  /// The local core can read and write the open folder in the desktop shell,
  /// even though the browser's File System Access API is unavailable there.
  const fsLocalTarget = source?.kind === 'fs' && localMode && token !== ''
  const canReceive =
    fsLocalTarget || (directionMode === 'directory' && currentHandle !== null)
  const uploadBlock = uploadBlockReason({
    selected: localSelection.size,
    remotePath: listing?.path ?? '',
    hasSource: source !== null,
    busy: uploadBusy,
  })
  const downloadBlock = downloadBlockReason({
    selected: remoteSelection.size,
    busy: bulkBusy,
    mode: directionMode,
    hasTarget: canReceive,
  })
  const uploadDisabled = uploadBlock !== null
  const downloadDisabled = downloadBlock !== null
  const allLocalSelected =
    localRows.length > 0 && localSelection.size === localRows.length
  const allRemoteSelected =
    !!listing && listing.entries.length > 0 &&
    remoteSelection.size === listing.entries.length

  return (
    <div
      data-local-input="true"
      data-file-drop="true"
      className="absolute inset-y-0 right-0 z-40 flex w-[min(96vw,980px)] flex-col border-l border-line bg-surface/95 shadow-2xl backdrop-blur-md"
      onPointerDown={(event) => event.stopPropagation()}
      onPointerUp={(event) => event.stopPropagation()}
      onDragOver={(event) => {
        event.preventDefault()
        event.stopPropagation()
      }}
      onDrop={(event: DragEvent) => {
        event.preventDefault()
        event.stopPropagation()
        if (event.dataTransfer) {
          void collectDropEntries(event.dataTransfer).then(applyDropped)
        }
      }}
    >
      <div className="flex h-11 shrink-0 items-center gap-2 border-b border-line-soft px-3">
        <FolderOpen size={15} className="text-accent" />
        <span className="text-[13px] font-medium">{t('files.title')}</span>
        <span className="text-[10px] text-muted">{t('files.remoteHint')}</span>
        <button
          type="button"
          onClick={onClose}
          className="ml-auto grid h-7 w-7 place-items-center rounded-md text-muted transition hover:bg-panel hover:text-ink"
          aria-label={t('files.close')}
          title={t('files.close')}
        >
          <X size={15} />
        </button>
      </div>

      {error ? (
        <div className="mx-2 mt-2 rounded-lg border border-danger/35 bg-danger/10 px-3 py-2 text-[11px] text-danger">
          {error}
        </div>
      ) : null}
      {clipboardNote ? (
        <div className="mx-2 mt-2 rounded-lg border border-accent/30 bg-accent-soft px-3 py-2 text-[11px] text-accent">
          {clipboardNote}
        </div>
      ) : null}

      <div className="grid min-h-0 flex-1 grid-cols-[minmax(0,1fr)_74px_minmax(0,1fr)] gap-2 p-2">
        {/* ------------------------------------------------------- local pane */}
        <section className="flex min-h-0 min-w-0 flex-col rounded-xl border border-line-soft bg-panel/30">
          <div className="flex shrink-0 items-center gap-1 border-b border-line-soft px-1.5 py-1.5">
            <span className="shrink-0 text-[10px] font-semibold uppercase tracking-[0.14em] text-muted">
              {t('files.local')}
            </span>
            {source?.kind === 'fs' && source.roots.length > 0 ? (
              <select
                value={localActiveRoot}
                onChange={(event) => {
                  if (event.target.value !== '') {
                    void loadLocalDirectory(event.target.value)
                  }
                }}
                className="field h-6 max-w-[136px] shrink-0 truncate !py-0 text-[11px] leading-6"
                aria-label={t('files.roots')}
                title={t('files.roots')}
              >
                <option value="" disabled>
                  {t('files.roots')}
                </option>
                {source.roots.map((root) => (
                  <option key={root.path} value={root.path}>
                    {rootLabel(t, root)}
                  </option>
                ))}
              </select>
            ) : null}
            <button
              type="button"
              onClick={localUp}
              disabled={!source}
              className="ml-auto grid h-6 w-6 shrink-0 place-items-center rounded text-muted transition hover:bg-panel hover:text-ink disabled:opacity-40"
              aria-label={t('files.up')}
              title={t('files.up')}
            >
              <ChevronUp size={13} />
            </button>
            <button
              type="button"
              onClick={localHome}
              disabled={!source}
              className="grid h-6 w-6 shrink-0 place-items-center rounded text-muted transition hover:bg-panel hover:text-ink disabled:opacity-40"
              aria-label={t('files.home')}
              title={t('files.homeLocalTooltip')}
            >
              <Home size={12} />
            </button>
            <button
              type="button"
              onClick={refreshLocal}
              disabled={!source}
              className="grid h-6 w-6 shrink-0 place-items-center rounded text-muted transition hover:bg-panel hover:text-ink disabled:opacity-40"
              aria-label={t('files.refresh')}
              title={t('files.refresh')}
            >
              <RefreshCw size={11} />
            </button>
          </div>
          <div className="flex shrink-0 items-center gap-1 px-1.5 pt-1.5">
            <input
              value={localInput}
              spellCheck={false}
              onChange={(event) => setLocalInput(event.target.value)}
              onKeyDown={(event) => {
                if (event.key === 'Enter') {
                  event.preventDefault()
                  openLocalPath()
                } else if (event.key === 'Escape') {
                  setLocalInput(localLabel || readStored(LOCAL_PATH_KEY))
                }
              }}
              placeholder={
                localMode
                  ? t('files.pathPlaceholderLocal')
                  : t('files.pathPlaceholderLocalBrowser')
              }
              className="field min-w-0 flex-1 font-mono text-[10px]"
              aria-label={t('files.localPath')}
            />
            <button
              type="button"
              onClick={chooseLocalDirectory}
              className="flex h-7 shrink-0 items-center gap-1 rounded-md px-1.5 text-[9px] text-muted transition hover:bg-panel hover:text-accent"
              aria-label={t('files.selectDir')}
              title={t('files.selectDirTooltip')}
            >
              <FolderOpen size={12} />
              {t('files.selectDirShort')}
            </button>
            <button
              type="button"
              onClick={() => fileInputRef.current?.click()}
              className="flex h-7 shrink-0 items-center gap-1 rounded-md px-1.5 text-[9px] text-muted transition hover:bg-panel hover:text-accent"
              aria-label={t('files.selectFiles')}
              title={t('files.selectFilesTooltip')}
            >
              <FileIcon size={12} />
              {t('files.selectFilesShort')}
            </button>
            <input
              // A ref callback sets the attribute when the node actually
              // mounts. The old `useEffect(..., [open])` ran once before the
              // input existed (the panel returns null while closed), so the
              // control silently stayed a file picker.
              ref={(node) => {
                directoryInputRef.current = node
                if (node !== null) {
                  node.setAttribute('webkitdirectory', '')
                  node.setAttribute('directory', '')
                }
              }}
              type="file"
              multiple
              className="hidden"
              onChange={(event) => {
                applyInputFiles(event.target.files, true)
                event.target.value = ''
              }}
            />
            <input
              ref={fileInputRef}
              type="file"
              multiple
              className="hidden"
              onChange={(event) => {
                applyInputFiles(event.target.files, false)
                event.target.value = ''
              }}
            />
            <button
              type="button"
              onClick={syncLocalToRemote}
              disabled={source?.kind !== 'fs' || !listing}
              className="grid h-7 w-7 shrink-0 place-items-center rounded-md text-muted transition hover:bg-panel hover:text-accent disabled:opacity-40"
              aria-label={t('files.syncToRemote')}
              title={
                source?.kind === 'fs'
                  ? t('files.syncToRemote')
                  : t('files.syncNeedsLocalFs')
              }
            >
              <ArrowLeftRight size={12} />
            </button>
          </div>
          {source?.kind === 'fs' && source.path !== '' &&
          localCrumbs.length > 0 ? (
            <div className="flex shrink-0 items-center gap-0.5 overflow-x-auto px-1.5 pb-1 text-[9px] text-muted">
              {localCrumbs.map((crumb) => (
                <span key={crumb.path} className="flex shrink-0 items-center">
                  {crumb.name !== '/' ? (
                    <span className="px-0.5 text-line">›</span>
                  ) : null}
                  <button
                    type="button"
                    onClick={() => void loadLocalDirectory(crumb.path)}
                    className="max-w-[120px] truncate rounded px-1 transition hover:bg-panel hover:text-accent"
                    title={crumb.path}
                  >
                    {crumb.name}
                  </button>
                </span>
              ))}
            </div>
          ) : null}
          <div className="flex shrink-0 items-center gap-1.5 px-2 py-1">
            <label className="flex items-center gap-1 text-[10px] text-muted">
              <input
                type="checkbox"
                checked={allLocalSelected}
                disabled={localRows.length === 0}
                onChange={toggleAllLocal}
                title={t('files.selectAll')}
                className="h-3 w-3"
              />
              {t('files.selectAll')}
            </label>
            {localSelection.size > 0 ? (
              <span className="chip !px-1.5 !py-0 text-[9px]">
                {t('files.selectedCount', { count: localSelection.size })}
              </span>
            ) : null}
            {displayedLocalNote ? (
              <span
                className="ml-auto min-w-0 truncate text-[9px] text-muted"
                title={displayedLocalNote}
              >
                {displayedLocalNote}
              </span>
            ) : null}
          </div>

          <div className="min-h-0 flex-1 overflow-y-auto px-1.5 pb-1.5">
            {!source ? (
              <div className="flex h-full flex-col items-center justify-center rounded-lg border border-dashed border-line px-3 py-6 text-center">
                <Upload size={18} className="text-accent/70" />
                <div className="mt-2 text-[11px] text-ink">
                  {t('files.dropHere')}
                </div>
                <div className="mt-1 text-[10px] leading-relaxed text-muted">
                  {t('files.pickLocalHint')}
                </div>
              </div>
            ) : localLoading ? (
              <div className="py-8 text-center text-[11px] text-muted">
                {t('files.loading')}
              </div>
            ) : localRows.length === 0 ? (
              <div className="py-8 text-center text-[11px] text-muted">
                {t('files.empty')}
              </div>
            ) : (
              <ul className="space-y-0.5">
                {localRows.map((row) => {
                  const selected = localSelection.has(row.key)
                  return (
                    <li
                      key={row.key}
                      className={
                        'flex items-center gap-1 rounded-md px-1.5 py-1 transition ' +
                        (selected ? 'bg-accent-soft' : 'hover:bg-panel/70')
                      }
                    >
                      <input
                        type="checkbox"
                        checked={selected}
                        onChange={() => toggleLocal(row.key)}
                        className="h-3 w-3 shrink-0"
                        aria-label={row.name}
                      />
                      <button
                        type="button"
                        onClick={() => {
                          if (row.dir) navigateLocalDirectory(row)
                          else toggleLocal(row.key)
                        }}
                        className="flex min-w-0 flex-1 items-center gap-2 text-left"
                        title={row.dir ? t('files.openDir') : row.name}
                      >
                        {row.dir ? (
                          <Folder size={13} className="shrink-0 text-accent/80" />
                        ) : (
                          <FileIcon size={13} className="shrink-0 text-muted" />
                        )}
                        <span className="min-w-0 flex-1 truncate text-[11px] text-ink">
                          {row.name}
                        </span>
                        {row.dir ? null : (
                          <span className="shrink-0 text-[9px] text-muted">
                            {formatBytes(row.size)}
                          </span>
                        )}
                      </button>
                    </li>
                  )
                })}
              </ul>
            )}
          </div>
        </section>

        {/* -------------------------------------------------------- actions */}
        <div className="flex min-h-0 flex-col items-stretch justify-center gap-2">
          <button
            type="button"
            onClick={() => void sendOver()}
            disabled={uploadDisabled}
            title={
              uploadBlock
                ? blockText(t, uploadBlock, 'up')
                : t('files.sendToHint')
            }
            className="btn btn-primary flex h-12 flex-col items-center justify-center gap-0.5 px-1 text-[10px] leading-tight"
          >
            <ArrowRight size={15} />
            <span className="text-center">{t('files.sendTo')}</span>
          </button>
          <button
            type="button"
            onClick={() => void receiveBack()}
            disabled={downloadDisabled}
            title={
              downloadBlock
                ? blockText(t, downloadBlock, 'down')
                : t('files.receiveFromHint')
            }
            className="btn flex h-12 flex-col items-center justify-center gap-0.5 px-1 text-[10px] leading-tight"
          >
            <ArrowLeft size={15} />
            <span className="text-center">{t('files.receiveFrom')}</span>
          </button>
        </div>

        {/* ------------------------------------------------------ remote pane */}
        <section className="flex min-h-0 min-w-0 flex-col rounded-xl border border-line-soft bg-panel/30">
          <div className="flex shrink-0 items-center gap-1 border-b border-line-soft px-1.5 py-1.5">
            <span className="shrink-0 text-[10px] font-semibold uppercase tracking-[0.14em] text-muted">
              {t('files.remote')}
            </span>
            {listing && listing.roots.length > 0 ? (
              <select
                value={listing.path === '' ? '__roots__' : remoteActiveRoot}
                onChange={(event) => {
                  const value = event.target.value
                  browseRemote(value === '__roots__' ? '' : value)
                }}
                className="field h-6 max-w-[136px] shrink-0 truncate !py-0 text-[11px] leading-6"
                aria-label={t('files.roots')}
                title={t('files.roots')}
              >
                <option value="__roots__">{t('files.roots')}</option>
                {listing.roots.map((root) => (
                  <option key={root.path} value={root.path}>
                    {rootLabel(t, root)}
                  </option>
                ))}
              </select>
            ) : null}
            <button
              type="button"
              onClick={remoteUp}
              disabled={!listing || listing.path === ''}
              className="ml-auto grid h-6 w-6 shrink-0 place-items-center rounded text-muted transition hover:bg-panel hover:text-ink disabled:opacity-40"
              aria-label={t('files.up')}
              title={t('files.up')}
            >
              <ChevronUp size={13} />
            </button>
            <button
              type="button"
              onClick={remoteHome}
              disabled={!listing}
              className="grid h-6 w-6 shrink-0 place-items-center rounded text-muted transition hover:bg-panel hover:text-ink disabled:opacity-40"
              aria-label={t('files.home')}
              title={t('files.homeRemoteTooltip')}
            >
              <Home size={12} />
            </button>
            <button
              type="button"
              onClick={() => browseRemote(listing?.path ?? remoteInput)}
              className="grid h-6 w-6 shrink-0 place-items-center rounded text-muted transition hover:bg-panel hover:text-ink"
              aria-label={t('files.refresh')}
              title={t('files.refresh')}
            >
              <RefreshCw size={11} />
            </button>
          </div>
          <div className="flex shrink-0 items-center gap-1 px-1.5 pt-1.5">
            <input
              value={remoteInput}
              spellCheck={false}
              onChange={(event) => setRemoteInput(event.target.value)}
              onKeyDown={(event) => {
                if (event.key === 'Enter') {
                  event.preventDefault()
                  openRemotePath()
                } else if (event.key === 'Escape') {
                  setRemoteInput(listing?.path ?? '')
                }
              }}
              placeholder={t('files.pathPlaceholderRemote')}
              className="field min-w-0 flex-1 font-mono text-[10px]"
              aria-label={t('files.remotePath')}
            />
            <button
              type="button"
              onClick={openRemotePath}
              className="btn h-7 gap-1 px-2 text-[10px]"
            >
              <FolderOpen size={12} />
              {t('files.browse')}
            </button>
            <button
              type="button"
              onClick={syncRemoteToLocal}
              disabled={!listing || listing.path === '' || !localMode ||
                token === ''}
              className="grid h-7 w-7 shrink-0 place-items-center rounded-md text-muted transition hover:bg-panel hover:text-accent disabled:opacity-40"
              aria-label={t('files.syncToLocal')}
              title={t('files.syncToLocal')}
            >
              <ArrowLeftRight size={12} />
            </button>
          </div>
          {listing && listing.path !== '' && remoteCrumbs.length > 0 ? (
            <div className="flex shrink-0 items-center gap-0.5 overflow-x-auto px-1.5 py-1 text-[9px] text-muted">
              <button
                type="button"
                onClick={() => browseRemote('')}
                className="shrink-0 rounded px-1 transition hover:bg-panel hover:text-accent"
                title={t('files.roots')}
              >
                {t('files.rootsShort')}
              </button>
              {remoteCrumbs.map((crumb) => (
                <span key={crumb.path} className="flex shrink-0 items-center">
                  <span className="px-0.5 text-line">›</span>
                  <button
                    type="button"
                    onClick={() => browseRemote(crumb.path)}
                    className="max-w-[120px] truncate rounded px-1 transition hover:bg-panel hover:text-accent"
                    title={crumb.path}
                  >
                    {crumb.name}
                  </button>
                </span>
              ))}
            </div>
          ) : null}
          <div className="flex shrink-0 items-center gap-1.5 px-2 py-1">
            <label className="flex items-center gap-1 text-[10px] text-muted">
              <input
                type="checkbox"
                checked={allRemoteSelected}
                disabled={!listing || listing.path === '' ||
                  listing.entries.length === 0}
                onChange={toggleAllRemote}
                title={t('files.selectAll')}
                className="h-3 w-3"
              />
              {t('files.selectAll')}
            </label>
            {remoteSelection.size > 0 ? (
              <span className="chip !px-1.5 !py-0 text-[9px]">
                {t('files.selectedCount', { count: remoteSelection.size })}
              </span>
            ) : null}
          </div>

          {clipboardFiles.length > 0 ? (
            <div className="mx-1.5 mb-1 shrink-0 rounded-lg border border-accent/30 bg-accent-soft/60 p-1.5">
              <div className="flex items-center gap-1.5">
                <ClipboardCopy size={11} className="shrink-0 text-accent" />
                <span className="min-w-0 flex-1 truncate text-[10px] font-medium text-accent">
                  {t('files.clipboardTitle')}
                </span>
                <span className="chip !px-1 !py-0 text-[9px]">
                  {clipboardFiles.length}
                </span>
                <button
                  type="button"
                  onClick={startClipboardDownloads}
                  disabled={
                    !clipboardFiles.some(
                      (file) => typeof file.path === 'string' && file.path !== '',
                    )
                  }
                  className="btn h-5 px-1.5 text-[9px]"
                  title={
                    clipboardMode === 'local-app'
                      ? t('files.clipboardLocalApp')
                      : clipboardMode === 'auto'
                        ? t('files.clipboardAuto')
                        : t('files.clipboardHint')
                  }
                >
                  {t('files.receiveAll')}
                </button>
              </div>
              {clipboardMode === 'local-app' ? (
                // A plain browser page cannot write the OS clipboard; say so
                // and point at "Receive all", which downloads the same files
                // through the ordinary transfer channel.
                <p className="mt-1 text-[9px] leading-snug text-muted">
                  {t('files.clipboardLocalApp')}
                </p>
              ) : null}
              <ul className="mt-1 max-h-20 space-y-0.5 overflow-y-auto">
                {clipboardFiles.map((file, index) => (
                  <li key={file.path ?? `${file.name}:${index}`}>
                    <button
                      type="button"
                      disabled={typeof file.path !== 'string'}
                      onClick={() => {
                        if (typeof file.path === 'string') {
                          startQuickDownload(file.path, file.name)
                        }
                      }}
                      className="flex w-full items-center gap-1.5 rounded px-1 py-0.5 text-left transition hover:bg-panel/60"
                      title={file.path ?? file.name}
                    >
                      <FileIcon size={10} className="shrink-0 text-muted" />
                      <span className="min-w-0 flex-1 truncate text-[10px] text-ink">
                        {file.name}
                      </span>
                      <span className="shrink-0 text-[9px] text-muted">
                        {formatBytes(file.size)}
                      </span>
                    </button>
                  </li>
                ))}
              </ul>
            </div>
          ) : null}

          <div className="min-h-0 flex-1 overflow-y-auto px-1.5 pb-1.5">
            {loading && !listing ? (
              <div className="py-8 text-center text-[11px] text-muted">
                {t('files.loading')}
              </div>
            ) : !listing ? (
              <div className="py-8 text-center text-[11px] text-muted">
                {t('files.needRemoteListing')}
              </div>
            ) : listing.entries.length === 0 ? (
              <div className="py-8 text-center text-[11px] text-muted">
                {t('files.empty')}
              </div>
            ) : (
              <ul className="space-y-0.5">
                {listing.entries.map((entry) => {
                  const selected = remoteSelection.has(entry.name)
                  const rootsView = listing.path === ''
                  const entryPath =
                    entry.path ?? joinPath(listing.path, entry.name)
                  return (
                    <li
                      key={entry.name}
                      draggable={!rootsView}
                      onDragStart={(event) =>
                        onRemoteDragStart(event, entry, entryPath)
                      }
                      onPointerDown={(event) =>
                        onRemotePointerDown(event, entry, entryPath)
                      }
                      onClickCapture={(event) => {
                        if (!dragOutSuppressClick.current) return
                        dragOutSuppressClick.current = false
                        event.preventDefault()
                        event.stopPropagation()
                      }}
                      className={
                        'flex items-center gap-1 rounded-md px-1.5 py-1 transition ' +
                        (selected ? 'bg-accent-soft' : 'hover:bg-panel/70')
                      }
                    >
                      <input
                        type="checkbox"
                        checked={selected}
                        disabled={rootsView}
                        onChange={() => toggleRemote(entry)}
                        className="h-3 w-3 shrink-0"
                        aria-label={entry.name}
                      />
                      <button
                        type="button"
                        onClick={() => {
                          if (entry.dir) {
                            browseRemote(entryPath)
                          } else {
                            toggleRemote(entry)
                          }
                        }}
                        className="flex min-w-0 flex-1 items-center gap-2 text-left"
                        title={entry.dir ? entryPath : entry.name}
                      >
                        {entry.dir ? (
                          <Folder size={13} className="shrink-0 text-accent/80" />
                        ) : (
                          <FileIcon size={13} className="shrink-0 text-muted" />
                        )}
                        <span className="min-w-0 flex-1 truncate text-[11px] text-ink">
                          {entry.name}
                        </span>
                        {entry.link ? (
                          <Link2 size={10} className="shrink-0 text-muted" />
                        ) : null}
                        {entry.kind && rootsView ? (
                          <span className="shrink-0 text-[9px] text-muted">
                            {rootKindText(t, entry.kind)}
                          </span>
                        ) : null}
                        {entry.dir ? null : (
                          <span className="shrink-0 text-[9px] text-muted">
                            {formatBytes(entry.size)}
                          </span>
                        )}
                      </button>
                      {entry.dir ? null : (
                        <button
                          type="button"
                          onClick={() =>
                            startQuickDownload(
                              entryPath,
                              entry.name,
                              entry.mtime,
                            )
                          }
                          className="grid h-6 w-6 shrink-0 place-items-center rounded text-muted transition hover:bg-panel hover:text-accent"
                          aria-label={t('files.download')}
                          title={t('files.download')}
                        >
                          <ArrowDownToLine size={11} />
                        </button>
                      )}
                    </li>
                  )
                })}
              </ul>
            )}
          </div>
        </section>
      </div>

      {dragOut ? (
        <div className="mx-2 mb-1 shrink-0 rounded-lg border border-accent/35 bg-accent-soft px-2.5 py-1.5">
          <div className="flex items-center gap-2 text-[10px] text-ink">
            <Upload size={12} className="shrink-0 text-accent" />
            <span className="min-w-0 flex-1 truncate">
              {dragOut.phase === 'preparing'
                ? t('files.dragOutTitle', { name: dragOut.name })
                : dragOut.phase === 'ready'
                  ? t('files.dragOutReady')
                  : t('files.dragOutFailed')}
            </span>
            {dragOut.phase === 'preparing' ? (
              <button
                type="button"
                onClick={cancelDragOut}
                className="shrink-0 rounded border border-line px-1.5 py-0.5 text-[9px] text-muted transition hover:bg-panel hover:text-ink"
              >
                {t('common.cancel')}
              </button>
            ) : null}
          </div>
          {dragOut.phase === 'preparing' ? (
            <>
              <div className="mt-1 h-1 overflow-hidden rounded-full bg-line">
                <div
                  className="h-full rounded-full bg-accent transition-all"
                  style={{ width: `${dragOut.percent}%` }}
                />
              </div>
              {dragOut.slow ? (
                <div className="mt-1 text-[9px] leading-snug text-muted">
                  {t('files.dragOutSlow')}
                </div>
              ) : null}
            </>
          ) : null}
        </div>
      ) : null}

      {download ? (
        <div className="mx-2 mb-1 shrink-0 rounded-lg border border-accent/35 bg-accent-soft px-2.5 py-1.5">
          <div className="flex items-center gap-2 text-[10px] text-ink">
            <ArrowDownToLine size={12} className="text-accent" />
            <span className="min-w-0 flex-1 truncate">{download.name}</span>
            <span className="shrink-0 text-muted">
              {downloadPhaseText(download.phase, t)}
            </span>
          </div>
          <div className="mt-1 h-1 overflow-hidden rounded-full bg-line">
            <div
              className="h-full rounded-full bg-accent transition-all"
              style={{
                width: `${
                  download.phase === 'saved'
                    ? 100
                    : download.size > 0
                      ? Math.min(
                          100,
                          Math.round(
                            (download.received / download.size) * 100,
                          ),
                        )
                      : 0
                }%`,
              }}
            />
          </div>
        </div>
      ) : null}

      {saveReady.length > 0 ? (
        <div className="mx-2 mb-1 shrink-0 space-y-1">
          {saveReady.map((item) => (
            <div
              key={item.key}
              className="flex items-center gap-2 rounded-lg border border-line bg-panel/70 px-2.5 py-1.5"
            >
              <FileIcon size={13} className="shrink-0 text-muted" />
              <span
                className="min-w-0 flex-1 truncate text-[11px] text-ink"
                title={item.name}
              >
                {item.name}
              </span>
              <span className="shrink-0 text-[9px] text-muted">
                {formatBytes(item.size)}
              </span>
              <button
                type="button"
                onClick={() => saveReadyFile(item)}
                className="btn btn-primary h-6 gap-1 px-2 text-[10px]"
              >
                <Save size={11} />
                {t('files.save')}
              </button>
              <button
                type="button"
                onClick={() =>
                  setSaveReady((current) =>
                    current.filter((entry) => entry.key !== item.key),
                  )
                }
                className="grid h-6 w-6 shrink-0 place-items-center rounded text-muted transition hover:bg-panel hover:text-ink"
                aria-label={t('files.dismiss')}
                title={t('files.dismiss')}
              >
                <X size={11} />
              </button>
            </div>
          ))}
        </div>
      ) : null}

      {tasks.length > 0 ? (
        <div className="shrink-0 border-t border-line-soft bg-panel/40">
          <div className="flex items-center gap-2 px-3 py-1">
            <span className="text-[10px] font-semibold uppercase tracking-[0.14em] text-muted">
              {t('files.transfers')}
            </span>
            <button
              type="button"
              onClick={clearFinished}
              disabled={!tasks.some((task) => task.finished)}
              className="ml-auto text-[10px] text-muted transition hover:text-ink disabled:opacity-40"
            >
              {t('files.clearFinished')}
            </button>
          </div>
          <div className="max-h-52 space-y-1.5 overflow-y-auto px-2 pb-2">
            {tasks.map((task) => {
              const progress = aggregateProgress(task.children)
              const summary = summarizeTask(task.children, task.skipped)
              const status = task.finished
                ? task.phase
                : taskStatusFromChildren(task.children, task.cancelled)
              const percent = task.phase === 'preparing'
                ? 0
                : progress.percent
              const color = task.phase === 'error' || status === 'error'
                ? 'bg-danger'
                : task.phase === 'cancelled' || status === 'cancelled'
                  ? 'bg-muted'
                  : 'bg-accent'
              return (
                <div
                  key={task.key}
                  className="rounded-lg border border-line-soft bg-surface/70 px-2.5 py-1.5"
                >
                  <div className="flex items-center gap-1.5">
                    {task.direction === 'up' ? (
                      <Upload size={12} className="shrink-0 text-accent" />
                    ) : (
                      <Download size={12} className="shrink-0 text-accent" />
                    )}
                    <span
                      className="min-w-0 flex-1 truncate text-[11px] text-ink"
                      title={task.name}
                    >
                      {task.name}
                    </span>
                    {task.children.length > 1 || task.dirsTotal > 0 ? (
                      <span className="shrink-0 text-[9px] text-muted">
                        {progress.doneFiles}/{progress.totalFiles}
                      </span>
                    ) : null}
                    <span className="shrink-0 text-[9px] text-muted">
                      {formatBytes(progress.doneBytes)} /{' '}
                      {formatBytes(progress.totalBytes)}
                    </span>
                    {!task.finished && task.speed > 0 ? (
                      <span className="shrink-0 text-[9px] text-muted">
                        {t('files.rate', {
                          rate: formatBytes(task.speed),
                        })}
                      </span>
                    ) : null}
                    {!task.finished ? (
                      <button
                        type="button"
                        onClick={() => cancelTask(task.key)}
                        className="grid h-5 w-5 shrink-0 place-items-center rounded text-muted transition hover:bg-panel hover:text-danger"
                        aria-label={t('files.cancel')}
                        title={t('files.cancel')}
                      >
                        <X size={11} />
                      </button>
                    ) : (
                      <button
                        type="button"
                        onClick={() => removeTask(task.key)}
                        className="grid h-5 w-5 shrink-0 place-items-center rounded text-muted transition hover:bg-panel hover:text-ink"
                        aria-label={t('files.clearTask')}
                        title={t('files.clearTask')}
                      >
                        <X size={11} />
                      </button>
                    )}
                  </div>
                  <div className="mt-1 h-1 overflow-hidden rounded-full bg-line-soft">
                    <div
                      className={`h-full rounded-full transition-all ${color}`}
                      style={{ width: `${percent}%` }}
                    />
                  </div>
                  <div className="mt-1 flex flex-wrap items-center gap-x-2 gap-y-0.5 text-[9px] text-muted">
                    {task.phase === 'preparing' ? (
                      <span>{t('files.preparing')}</span>
                    ) : task.phase === 'cancelled' || status === 'cancelled' ? (
                      <span>{t('files.cancelled')}</span>
                    ) : task.phase === 'error' || status === 'error' ? (
                      <span className="text-danger">{t('files.failed')}</span>
                    ) : task.finished ? (
                      <span>{t('files.done')}</span>
                    ) : (
                      <span>{t('files.sending')}</span>
                    )}
                    {task.finished ? (
                      <span>
                        {t('files.summary', {
                          done: summary.done,
                          failed: summary.failed,
                          cancelled: summary.cancelled,
                        })}
                        {summary.skipped > 0
                          ? ` · ${t('files.skippedCount', {
                              count: summary.skipped,
                            })}`
                          : ''}
                      </span>
                    ) : null}
                    {task.dirsTotal > 0 ? (
                      <span>
                        {t('files.dirsSummary', {
                          done: task.dirsDone,
                          total: task.dirsTotal,
                        })}
                      </span>
                    ) : null}
                    {task.note ? (
                      <span className="text-danger/90">{task.note}</span>
                    ) : null}
                    {task.finished && task.remotePaths.length > 0 ? (
                      <button
                        type="button"
                        onClick={() =>
                          connectionRef.current?.sendFileAction(
                            'clipboard-set',
                            { paths: task.remotePaths },
                          )
                        }
                        className="btn ml-auto h-5 gap-1 px-1.5 text-[9px]"
                      >
                        <ClipboardPaste size={10} />
                        {t('files.copyToClipboard')}
                      </button>
                    ) : null}
                  </div>
                </div>
              )
            })}
          </div>
        </div>
      ) : null}
    </div>
  )
}
