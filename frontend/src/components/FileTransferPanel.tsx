import {
  ArrowDownToLine,
  ArrowLeft,
  ArrowRight,
  ChevronUp,
  ClipboardCopy,
  ClipboardPaste,
  Download,
  File as FileIcon,
  Folder,
  FolderOpen,
  Home,
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
} from 'react'
import { useTranslation } from 'react-i18next'
import type { TFunction } from 'i18next'
import { coreBaseUrl, fetchLocalDirectory, type FsEntry } from '../lib/api'
import { clipboardFilesMode } from '../lib/clipboard-local'
import {
  collectDropEntries,
  onDroppedFiles,
  type StagedFile,
} from '../lib/file-drop'
import type {
  ClipboardFileEntry,
  FileEntry,
  FileMessage,
} from '../lib/client'
import type { SessionConnection } from '../lib/session-connection'
import {
  acceptDownloadChunk,
  downloadComplete,
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
import {
  aggregateProgress,
  buildUploadPlan,
  downloadBlockReason,
  downloadModeFor,
  isUnsupportedAction,
  localEntriesAt,
  normalizeRelativePath,
  relativeUnder,
  resolveRemotePath,
  safeTargetSegments,
  sourceFromInputFiles,
  summarizeTask,
  taskStatusFromChildren,
  uploadBlockReason,
  type ChildStatus,
  type DownloadMode,
  type TransferBlock,
} from '../lib/file-transfer'
import { useAppStore } from '../store/app'

const downloadTimeoutMs = 30_000
const browseTimeoutMs = 15_000
const uploadReplyTimeoutMs = 30_000
const uploadDoneTimeoutMs = 90_000

const LOCAL_PATH_KEY = 'molink.files.localPath'
const REMOTE_PATH_KEY = 'molink.files.remotePath'

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

/// The remote listing uses the host's separator; every path the panel joins
/// keeps that separator so a Windows host never gets a mixed path.
function joinPath(base: string, name: string): string {
  if (base === '') return name
  const separator = base.includes('\\') ? '\\' : '/'
  const trimmed = base.replace(/[\\/]+$/, '')
  return trimmed + separator + name
}

function parentPath(path: string, root: string): string {
  if (!path || path === root) return root
  const separator = path.includes('\\') ? '\\' : '/'
  const trimmed = path.replace(/[\\/]+$/, '')
  const cut = trimmed.lastIndexOf(separator)
  if (cut <= 0) return root
  const parent = trimmed.slice(0, cut)
  const base = root.replace(/[\\/]+$/, '')
  if (base !== '' && parent !== base && !parent.startsWith(base + separator)) {
    return root
  }
  return parent
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

interface RemoteListing {
  path: string
  root: string
  entries: FileEntry[]
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
  file: File
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
      /// A folder listed by the local core (`/api/fs/list`). The browser can
      /// navigate it but cannot read its files, so uploads still need a picker.
      kind: 'fs'
      path: string
      parent: string
      root: string
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
  writable: FileSystemWritableFileStream | null
  parentDir: FileSystemDirectoryHandle | null
  createdFile: boolean
  requesting: boolean
  id: number
  size: number
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
}: {
  open: boolean
  onClose: () => void
  connection: SessionConnection | null
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
  finishBrowseRef.current = finishBrowse

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
  browsePumpRef.current = pumpBrowse

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
    job.timer = window.setTimeout(() => {
      if (downloadSection.current === job) {
        finishDownloadRef.current(job, t('files.incomplete'))
      }
    }, downloadTimeoutMs)
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
  }, [])

  const finishDownload = useCallback(
    (job: DownloadJob, failure?: string, cancelled = false) => {
      if (job.finishing) return
      job.finishing = true
      window.clearTimeout(job.timer)
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
        job.resolve()
        downloadPumpRef.current()
      }
      void settle()
    },
    [discardPartial, noteTaskSpeed, patchChild, t],
  )
  finishDownloadRef.current = finishDownload

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
  downloadPumpRef.current = pumpDownloads

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
      resolve: () => void
    }): DownloadJob => ({
      key: nextKey.current++,
      ...props,
      writable: null,
      parentDir: null,
      createdFile: false,
      requesting: false,
      id: 0,
      size: 0,
      received: 0,
      eofSeen: false,
      cancelled: false,
      finishing: false,
      committed: false,
      chunks: [],
      writeChain: Promise.resolve(),
      writeError: undefined,
      timer: 0,
    }),
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
      files: Array<{ path: string; name: string; childKey: string }>,
      targets: Array<Promise<FileSystemWritableFileStream> | null>,
      strategy: SaveStrategy,
      silent: boolean,
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
    (path: string, name: string) => {
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
        [{ path, name: suggested, childKey }],
        [pick.target],
        pick.strategy,
        false,
      )
    },
    [createTask, runQuickDownloads],
  )

  const startClipboardDownloads = useCallback(() => {
    const files = clipboardFiles
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
          } else if (message.path && message.entries) {
            finishBrowseRef.current({
              path: message.path,
              root: message.root ?? message.path,
              entries: message.entries,
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
              message.message ?? t('files.error'),
            )
            return
          }
          job.id = message.id
          job.size = size
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
            received: 0,
            size,
          })
          connectionRef.current?.sendFileAction('pull', { id: job.id })
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
      if (job.writable) {
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
    [armDownloadTimeout, patchProgress, t],
  )

  const messageRef = useRef(handleMessage)
  messageRef.current = handleMessage
  const chunkRef = useRef(handleChunk)
  chunkRef.current = handleChunk

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
  const loadLocalDirectory = useCallback(
    async (path: string) => {
      if (!localMode || token === '') {
        setError(t('files.localPathNeedsClient'))
        return
      }
      const loadToken = ++localLoadToken.current
      setLocalLoading(true)
      try {
        const result = await fetchLocalDirectory(token, path)
        if (loadToken !== localLoadToken.current) return
        setSource({
          kind: 'fs',
          path: result.path,
          parent: result.parent,
          root: result.root,
        })
        setFsEntries(result.entries)
        setHandleEntries([])
        setHandleLevels([])
        setSnapshotPath('')
        setLocalSelection(new Set())
        setLocalInput(result.path)
        storePath(LOCAL_PATH_KEY, result.path)
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

  const chooseLocalDirectory = useCallback(() => {
    setError(null)
    if (!canPickDirectory(window)) {
      const input = directoryInputRef.current
      // `webkitdirectory` is an attribute, not a JS-only flag. If the browser
      // ignored it the control silently becomes a file picker, which loses
      // the folder shape -- say so instead of pretending a folder was chosen.
      if (!supportsDirectoryInput(document)) {
        setLocalNote(t('files.dirPickerUnsupported'))
      }
      input?.click()
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
      chooseLocalDirectory()
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
    const current = listing?.path ?? remoteInput
    browseRemote(parentPath(current, listing?.root ?? ''))
  }, [browseRemote, listing, remoteInput])

  const remoteHome = useCallback(() => {
    browseRemote(listing?.root ?? '')
  }, [browseRemote, listing])

  const toggleRemote = useCallback((entry: FileEntry) => {
    setRemoteSelection((current) => {
      const next = new Map(current)
      if (next.has(entry.name)) next.delete(entry.name)
      else next.set(entry.name, entry)
      return next
    })
  }, [])

  const toggleAllRemote = useCallback(() => {
    if (!listing) return
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
      path: joinPath(listing.path, entry.name),
    }))
  }, [listing, remoteSelection])

  // ------------------------------------------------------------- upload run
  const sendOver = useCallback(async () => {
    if (!source || listing === null || localSelection.size === 0) return
    // A `/api/fs/list` pane knows names only; the browser still has no read
    // access to those bytes. Say how to actually send them instead of
    // starting a transfer that cannot produce a single file.
    if (source.kind === 'fs') {
      setError(t('files.fsListHint'))
      return
    }
    const dest = listing.path
    setError(null)
    setUploadBusy(true)
    try {
      let plan: UploadPlan
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

      if (plan.files.length === 0 && plan.dirs.length === 0) {
        setError(t('files.nothingToSend'))
        setUploadBusy(false)
        return
      }

      const selectedRows = localRows.filter((row) =>
        localSelection.has(row.rel))
      const taskName = selectedRows.length === 1
        ? selectedRows[0]!.name
        : t('files.itemsCount', { count: selectedRows.length })
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

          const answerPromise = waitUpload('offer', uploadReplyTimeoutMs)
          connectionRef.current?.sendFileAction('offer', {
            name: file.path,
            size: file.size,
            dest,
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
          const chunk = answer.chunk && answer.chunk > 0
            ? answer.chunk
            : 256 * 1024
          let offset = 0
          let failed = false
          while (offset < file.size) {
            if (run.cancelled) {
              connectionRef.current?.sendFileAction('cancel', { id })
              patchChild(taskKey, child.key, { status: 'cancelled' })
              failed = true
              break
            }
            const target = connectionRef.current
            if (!target) {
              patchChild(taskKey, child.key, {
                status: 'error',
                error: t('files.error'),
              })
              failed = true
              break
            }
            const end = Math.min(file.size, offset + chunk)
            const buffer = await file.file.slice(offset, end).arrayBuffer()
            if (run.cancelled) {
              connectionRef.current?.sendFileAction('cancel', { id })
              patchChild(taskKey, child.key, { status: 'cancelled' })
              failed = true
              break
            }
            target.sendFileChunk(id, offset, new Uint8Array(buffer))
            offset = end
            patchProgress(taskKey, child.key, offset)
            // Yield so a large file does not freeze the UI thread.
            await new Promise((resolve) => window.setTimeout(resolve, 0))
          }
          run.activeId = 0
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
    } catch (uploadError) {
      setUploadBusy(false)
      setError(errorText(uploadError) || t('files.error'))
    }
  }, [
    appendNote,
    createTask,
    finalizeTask,
    localRows,
    localSelection,
    patchChild,
    patchProgress,
    requestListing,
    snapshotPath,
    source,
    listing,
    t,
    updateTask,
    waitUpload,
  ])

  // ----------------------------------------------------------- download run
  const receiveBack = useCallback(async () => {
    if (!listing || !currentHandle || remoteSelection.size === 0) return
    const destination = currentHandle
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
        const segments = safeTargetSegments(dir, 'folder')
        try {
          await ensureDirectory(destination, segments)
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
        await new Promise<void>((resolve) => {
          const job = makeJob({
            path: file.path,
            name,
            taskKey,
            childKey: child.key,
            silent: true,
            strategy: 'picker',
            target: null,
            baseDir: destination,
            segments: parent,
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
    makeJob,
    patchChild,
    queueJob,
    remoteSelectedEntries,
    remoteSelection.size,
    requestListing,
    t,
    updateTask,
  ])

  // ------------------------------------------------------ connection cleanup
  // The unsubscribe below must not run on a language change, so the message
  // is read through a ref instead of putting `t` in the dependency list.
  const cancelledMessage = useRef(t('files.cancelled'))
  cancelledMessage.current = t('files.cancelled')

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
  const canReceive = directionMode === 'directory' && currentHandle !== null
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
  const fsSource = source?.kind === 'fs'
  const uploadDisabled = uploadBlock !== null || fsSource
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
              title={t('files.home')}
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
              placeholder={t('files.pathPlaceholderLocal')}
              className="field min-w-0 flex-1 font-mono text-[10px]"
              aria-label={t('files.localPath')}
            />
            <button
              type="button"
              onClick={chooseLocalDirectory}
              className="flex h-7 shrink-0 items-center gap-1 rounded-md px-1.5 text-[9px] text-muted transition hover:bg-panel hover:text-accent"
              aria-label={t('files.selectDir')}
              title={t('files.selectDir')}
            >
              <FolderOpen size={12} />
              {t('files.selectDirShort')}
            </button>
            <button
              type="button"
              onClick={() => fileInputRef.current?.click()}
              className="flex h-7 shrink-0 items-center gap-1 rounded-md px-1.5 text-[9px] text-muted transition hover:bg-panel hover:text-accent"
              aria-label={t('files.selectFiles')}
              title={t('files.selectFiles')}
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
          </div>
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
            {localNote ? (
              <span
                className="ml-auto min-w-0 truncate text-[9px] text-muted"
                title={localNote}
              >
                {localNote}
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
              fsSource
                ? t('files.fsListHint')
                : uploadBlock
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
            <button
              type="button"
              onClick={remoteUp}
              disabled={!listing}
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
              title={t('files.home')}
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
          </div>
          <div className="flex shrink-0 items-center gap-1.5 px-2 py-1">
            <label className="flex items-center gap-1 text-[10px] text-muted">
              <input
                type="checkbox"
                checked={allRemoteSelected}
                disabled={!listing || listing.entries.length === 0}
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
              <ul className="mt-1 max-h-20 space-y-0.5 overflow-y-auto">
                {clipboardFiles.map((file) => (
                  <li key={file.path}>
                    <button
                      type="button"
                      onClick={() => startQuickDownload(file.path, file.name)}
                      className="flex w-full items-center gap-1.5 rounded px-1 py-0.5 text-left transition hover:bg-panel/60"
                      title={file.path}
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
                  return (
                    <li
                      key={entry.name}
                      className={
                        'flex items-center gap-1 rounded-md px-1.5 py-1 transition ' +
                        (selected ? 'bg-accent-soft' : 'hover:bg-panel/70')
                      }
                    >
                      <input
                        type="checkbox"
                        checked={selected}
                        onChange={() => toggleRemote(entry)}
                        className="h-3 w-3 shrink-0"
                        aria-label={entry.name}
                      />
                      <button
                        type="button"
                        onClick={() => {
                          if (entry.dir && listing) {
                            browseRemote(joinPath(listing.path, entry.name))
                          } else {
                            toggleRemote(entry)
                          }
                        }}
                        className="flex min-w-0 flex-1 items-center gap-2 text-left"
                        title={entry.dir ? t('files.openDir') : entry.name}
                      >
                        {entry.dir ? (
                          <Folder size={13} className="shrink-0 text-accent/80" />
                        ) : (
                          <FileIcon size={13} className="shrink-0 text-muted" />
                        )}
                        <span className="min-w-0 flex-1 truncate text-[11px] text-ink">
                          {entry.name}
                        </span>
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
                            listing &&
                            startQuickDownload(
                              joinPath(listing.path, entry.name),
                              entry.name,
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
