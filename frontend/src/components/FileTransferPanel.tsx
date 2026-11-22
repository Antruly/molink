import {
  ArrowDownToLine,
  ChevronLeft,
  ChevronRight,
  ClipboardCopy,
  ClipboardPaste,
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
import { coreBaseUrl } from '../lib/api'
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
  mustAbortStream,
  suggestedFileName,
  type SaveStrategy,
} from '../lib/download-save'
import { useAppStore } from '../store/app'

const downloadTimeoutMs = 30_000

function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`
  if (bytes < 1024 * 1024 * 1024) return `${(bytes / 1024 / 1024).toFixed(1)} MB`
  return `${(bytes / 1024 / 1024 / 1024).toFixed(2)} GB`
}

interface UploadItem {
  key: number
  name: string
  size: number
  sent: number
  status: 'queued' | 'sending' | 'done' | 'error'
  path?: string
  error?: string
}

type DownloadPhase = 'waiting' | 'transferring' | 'saving' | 'saved'

interface DownloadItem {
  key: number
  id: number
  name: string
  size: number
  received: number
  phase: DownloadPhase
}

/// A download that finished into memory (the fallback path). It is saved only
/// when the user presses the button, so `anchor.click()` runs inside a real
/// user activation instead of the WebSocket callback.
interface ReadySave {
  key: number
  name: string
  size: number
  blob: Blob
}

/// One queued download. The picker is opened synchronously from the click, so
/// `target` is already pending before the transfer starts; the bytes flow only
/// after `createWritable()` resolved.
interface DownloadJob {
  key: number
  path: string
  name: string
  strategy: SaveStrategy
  target: Promise<FileSystemWritableFileStream> | null
  writable: FileSystemWritableFileStream | null
  /// True between sending `request` and the host's `sending` answer. Only
  /// this job may adopt that answer; another subsystem's transfer in flight
  /// (remote clipboard staging) must not be mistaken for it.
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
}

/// Opens the save dialog while the click's transient activation is still
/// live. The returned promise must not be awaited before `startDownload`
/// returns, or Chrome rejects it with "must be handling a user gesture".
function pickSaveTarget(name: string): {
  strategy: SaveStrategy
  target: Promise<FileSystemWritableFileStream> | null
} {
  if (!canUseFileSystemAccess(window)) return { strategy: 'button', target: null }
  try {
    const handle = window.showSaveFilePicker!({ suggestedName: name })
    return {
      strategy: 'picker',
      target: handle.then((file) => file.createWritable()),
    }
  } catch {
    // Missing activation or a browser that exposes the function but refuses
    // it for this origin: fall back to the in-memory path.
    return { strategy: 'button', target: null }
  }
}

/// "Receive all" gets one directory choice instead of one dialog per file.
/// Without a directory picker the files stay in memory and each gets a Save
/// button; opening several save dialogs from one click is worse than that.
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

function formatSize(bytes: number): string {
  if (bytes >= 1024 * 1024 * 1024) {
    return `${(bytes / (1024 * 1024 * 1024)).toFixed(1)} GB`
  }
  if (bytes >= 1024 * 1024) {
    return `${(bytes / (1024 * 1024)).toFixed(1)} MB`
  }
  if (bytes >= 1024) return `${Math.round(bytes / 1024)} KB`
  return `${bytes} B`
}

function joinPath(base: string, name: string): string {
  const separator = base.includes('\\') ? '\\' : '/'
  return base.endsWith(separator) ? base + name : base + separator + name
}

function parentPath(path: string, root: string): string {
  if (path === root) return root
  const separator = path.includes('\\') ? '\\' : '/'
  const cut = path.lastIndexOf(separator)
  if (cut <= 0) return root
  const parent = path.slice(0, cut)
  return parent.length < root.length ? root : parent
}

/// One breadcrumb per path component, starting at the listing root (the remote
/// home), so any level can be clicked and the whole bar can be typed into.
function pathCrumbs(
  path: string,
  root: string,
): Array<{ label: string; path: string }> {
  if (!path) return []
  const separator = path.includes('\\') ? '\\' : '/'
  const base = root.replace(/[\\/]+$/, '')
  const crumbs: Array<{ label: string; path: string }> = [
    { label: '~', path: base.length > 0 ? base : separator },
  ]
  if (base.length > 0 && path.startsWith(base) && path.length > base.length) {
    let accumulated = base
    for (const part of path.slice(base.length).split(separator)) {
      if (part.length === 0) continue
      accumulated = accumulated.endsWith(separator)
          ? accumulated + part
          : accumulated + separator + part
      crumbs.push({ label: part, path: accumulated })
    }
    return crumbs
  }
  if (!path.startsWith(base)) crumbs.push({ label: path, path })
  return crumbs
}

/// File transfer drawer: uploads land in the remote user's Downloads/moLink,
/// downloads browse the remote home folder one pull-sized chunk at a time.
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
  const [tab, setTab] = useState<'send' | 'receive'>('send')
  const [uploads, setUploads] = useState<UploadItem[]>([])
  const [listing, setListing] = useState<{
    path: string
    root: string
    entries: FileEntry[]
  } | null>(null)
  const [download, setDownload] = useState<DownloadItem | null>(null)
  const [saveReady, setSaveReady] = useState<ReadySave[]>([])
  const [error, setError] = useState<string | null>(null)
  const [loading, setLoading] = useState(false)
  const [clipboardFiles, setClipboardFiles] = useState<ClipboardFileEntry[]>([])
  const [clipboardNote, setClipboardNote] = useState<string | null>(null)
  const localMode = useAppStore((state) => state.localMode)
  const autoClipboardFiles = useAppStore((state) => state.remoteClipboardFiles)
  const clipboardMode = clipboardFilesMode(
    localMode,
    window.location.origin,
    coreBaseUrl(),
    autoClipboardFiles,
  )
  /// The breadcrumb turned into a text field (double click); a path that does
  /// not exist leaves the previous listing untouched and shows the error.
  const [editingPath, setEditingPath] = useState(false)
  const [pathDraft, setPathDraft] = useState('')
  const pathFieldRef = useRef<HTMLInputElement>(null)
  const crumbs = useMemo(
    () => pathCrumbs(listing?.path ?? '', listing?.root ?? ''),
    [listing?.path, listing?.root],
  )

  const queueRef = useRef<{ key: number; file: File; path: string }[]>([])
  const activeRef = useRef<{ key: number; file: File; path: string; id?: number } | null>(
    null,
  )
  // The remote folder uploads land in. Empty means the host default
  // (~/moLink); any path the host user can write is accepted.
  const [dest, setDest] = useState('~/moLink')
  const destRef = useRef(dest)
  useEffect(() => {
    destRef.current = dest
  }, [dest])
  const connectionRef = useRef<SessionConnection | null>(connection)
  const downloadRef = useRef<DownloadJob | null>(null)
  const downloadQueue = useRef<DownloadJob[]>([])
  /// `finishDownload` and the pump call each other; these refs break the
  /// useCallback dependency cycle while keeping one stable listener.
  const finishDownloadRef = useRef<(job: DownloadJob, failure?: string) => void>(
    () => undefined,
  )
  const pumpDownloadsRef = useRef<() => void>(() => undefined)
  const savedTimer = useRef<number | null>(null)
  const nextKey = useRef(1)
  const lastPaint = useRef(0)
  const inputRef = useRef<HTMLInputElement>(null)

  const clearSavedSoon = useCallback((key: number) => {
    if (savedTimer.current !== null) window.clearTimeout(savedTimer.current)
    savedTimer.current = window.setTimeout(() => {
      savedTimer.current = null
      setDownload((current) => (current?.key === key ? null : current))
    }, 2_500)
  }, [])

  const armDownloadTimeout = useCallback(() => {
    const job = downloadRef.current
    if (!job) return
    window.clearTimeout(job.timer)
    job.timer = window.setTimeout(() => {
      if (downloadRef.current === job) {
        finishDownloadRef.current(job, t('files.incomplete'))
      }
    }, downloadTimeoutMs)
  }, [t])

  /// Ends the active job. Success commits a streamed writable or hands the
  /// Blob to the Save button; failure always aborts a streamed writable so no
  /// half file is left behind.
  const finishDownload = useCallback(
    (job: DownloadJob, failure?: string) => {
      if (job.finishing) return
      job.finishing = true
      window.clearTimeout(job.timer)
      if (downloadRef.current === job) downloadRef.current = null

      const finish = async () => {
        if (failure !== undefined) {
          if (mustAbortStream(job.strategy, job.committed) && job.writable) {
            try {
              await job.writable.abort()
            } catch {
              // The stream may already be closed by a failed write.
            }
          }
          // Release the host's fixed download slot now instead of waiting for
          // its idle reclaim.
          if (job.id !== 0) {
            connectionRef.current?.sendFileAction('cancel', { id: job.id })
          }
          setError(failure)
          setDownload(null)
        } else if (job.strategy === 'picker' && job.writable) {
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
            setDownload((current) =>
              current?.key === job.key
                ? { ...current, phase: 'saved' }
                : current,
            )
            clearSavedSoon(job.key)
          } catch {
            if (mustAbortStream(job.strategy, job.committed)) {
              try {
                await job.writable.abort()
              } catch {
                // Already closed.
              }
            }
            setError(t('files.saveFailed'))
            setDownload(null)
          }
        } else if (maySaveFromButton(job.strategy, job, false)) {
          // No picker: keep the bytes in memory and let a click save them.
          const blob = new Blob(job.chunks as BlobPart[])
          setSaveReady((current) => [
            ...current.slice(-7),
            { key: job.key, name: job.name, size: job.size, blob },
          ])
          setDownload(null)
        } else {
          setError(t('files.incomplete'))
          setDownload(null)
        }
        pumpDownloadsRef.current()
      }
      void finish()
    },
    [clearSavedSoon, t],
  )
  finishDownloadRef.current = finishDownload

  /// Opens a picker immediately (still inside the click), then starts the
  /// request once the writable exists. A cancelled picker never opens a host
  /// transfer at all.
  const pumpDownloads = useCallback(() => {
    if (downloadRef.current) return
    const job = downloadQueue.current.shift()
    if (!job) return
    downloadRef.current = job

    const start = async () => {
      if (job.target) {
        try {
          job.writable = await job.target
        } catch (pickError) {
          if (isPickerCancellation(pickError)) {
            job.cancelled = true
            job.finishing = true
            window.clearTimeout(job.timer)
            if (downloadRef.current === job) downloadRef.current = null
            setDownload(null)
            pumpDownloadsRef.current()
            return
          }
          // The API exists but this origin/permission refused it; the memory
          // path still delivers the file.
          job.strategy = 'button'
          job.target = null
        }
      }
      if (job.cancelled || downloadRef.current !== job) {
        if (job.writable) {
          try {
            await job.writable.abort()
          } catch {
            // Nothing was committed.
          }
        }
        return
      }
      setDownload({
        key: job.key,
        id: 0,
        name: job.name,
        size: job.size,
        received: 0,
        phase: 'waiting',
      })
      job.requesting = true
      connectionRef.current?.sendFileAction('request', { path: job.path })
      armDownloadTimeout()
    }
    void start()
  }, [armDownloadTimeout])
  pumpDownloadsRef.current = pumpDownloads

  const queueDownload = useCallback(
    (
      path: string,
      name: string,
      pick: {
        strategy: SaveStrategy
        target: Promise<FileSystemWritableFileStream> | null
      },
    ) => {
      if (!path) return
      const key = nextKey.current++
      downloadQueue.current.push({
        key,
        path,
        name,
        strategy: pick.strategy,
        target: pick.target,
        writable: null,
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
      })
      pumpDownloadsRef.current()
    },
    [],
  )

  const startDownload = useCallback(
    (path: string, name?: string) => {
      if (!path) return
      const suggested = suggestedFileName(name ?? '', path)
      // Must happen in the click handler: the picker needs user activation.
      queueDownload(path, suggested, pickSaveTarget(suggested))
    },
    [queueDownload],
  )

  /// "Receive all": one directory choice for the whole batch when available.
  /// Otherwise every file takes the in-memory path and gets its own Save
  /// button, because several save dialogs from one click are blocked.
  const startDownloadAll = useCallback(
    (files: Array<{ path: string; name: string }>) => {
      if (files.length === 0) return
      const names = files.map((file) =>
        suggestedFileName(file.name, file.path),
      )
      const targets = pickDirectoryTargets(names)
      files.forEach((file, index) => {
        const suggested = names[index]!
        queueDownload(
          file.path,
          suggested,
          targets
            ? { strategy: 'picker', target: targets[index]! }
            : { strategy: 'button', target: null },
        )
      })
    },
    [queueDownload],
  )

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

  useEffect(() => {
    connectionRef.current = connection
    return () => {
      const current = downloadRef.current
      if (current) {
        window.clearTimeout(current.timer)
        current.cancelled = true
        // Unmounting or switching sessions abandons the transfer. Abort the
        // writable (leaving a half file would otherwise be committed) and
        // tell the host so its fixed download slot is released now.
        if (mustAbortStream(current.strategy, current.committed) &&
            current.writable) {
          void current.writable.abort().catch(() => undefined)
        }
        if (current.id !== 0) {
          connection?.sendFileAction('cancel', { id: current.id })
        }
      }
      for (const queued of downloadQueue.current) {
        queued.cancelled = true
        if (queued.target) {
          void queued.target
            .then((writable) => writable.abort())
            .catch(() => undefined)
        }
      }
      downloadRef.current = null
      downloadQueue.current = []
      connectionRef.current = null
      if (savedTimer.current !== null) {
        window.clearTimeout(savedTimer.current)
        savedTimer.current = null
      }
    }
  }, [connection])

  const paintUpload = useCallback((key: number, sent: number, force = false) => {
    const now = performance.now()
    if (!force && now - lastPaint.current < 120) return
    lastPaint.current = now
    setUploads((previous) =>
      previous.map((item) =>
        item.key === key ? { ...item, sent } : item,
      ),
    )
  }, [])

  const sendChunks = useCallback(
    async (key: number, file: File, id: number, chunk: number) => {
      let offset = 0
      setUploads((previous) =>
        previous.map((item) =>
          item.key === key ? { ...item, status: 'sending' } : item,
        ),
      )
      while (offset < file.size) {
        const target = connectionRef.current
        if (!target) break
        const end = Math.min(file.size, offset + chunk)
        const buffer = await file.slice(offset, end).arrayBuffer()
        target.sendFileChunk(id, offset, new Uint8Array(buffer))
        offset = end
        paintUpload(key, offset)
        // Yield so a large file does not freeze the UI thread.
        await new Promise((resolve) => window.setTimeout(resolve, 0))
      }
      paintUpload(key, offset, true)
      connectionRef.current?.sendClientLog(
        `upload chunks done id=${id} sent=${offset} size=${file.size}`,
      )
      connectionRef.current?.sendFileAction('complete', { id })
    },
    [paintUpload],
  )

  const startNext = useCallback(() => {
    if (activeRef.current !== null) return
    const next = queueRef.current.shift()
    if (!next) return
    activeRef.current = next
    setUploads((previous) =>
      previous.map((item) =>
        item.key === next.key ? { ...item, status: 'sending' } : item,
      ),
    )
    connectionRef.current?.sendClientLog(
      `upload offer name=${next.path} size=${next.file.size} dest=${destRef.current}`,
    )
    connectionRef.current?.sendFileAction('offer', {
      name: next.path,
      size: next.file.size,
      dest: destRef.current,
    })
  }, [])

  const finishActive = useCallback(
    (status: 'done' | 'error', path?: string, message?: string) => {
      const active = activeRef.current
      if (!active) return
      setUploads((previous) =>
        previous.map((item) =>
          item.key === active.key
            ? { ...item, status, path, error: message }
            : item,
        ),
      )
      activeRef.current = null
      window.setTimeout(startNext, 150)
    },
    [startNext],
  )

  const handleMessage = useCallback(
    (message: FileMessage) => {
      switch (message.action) {
        case 'accept': {
          const active = activeRef.current
          connectionRef.current?.sendClientLog(
            `upload accept id=${message.id ?? 0} chunk=${message.chunk ?? 0} ` +
              `active=${active ? 1 : 0}`,
          )
          if (!active || !message.id || active.id !== undefined) return
          active.id = message.id
          void sendChunks(
            active.key,
            active.file,
            message.id,
            message.chunk && message.chunk > 0
              ? message.chunk
              : 256 * 1024,
          )
          break
        }
        case 'done':
          if (message.ok !== false) {
            finishActive('done', message.path)
          } else {
            finishActive('error', undefined, message.message)
          }
          break
        case 'reject':
          finishActive('error', undefined, message.message)
          break
        case 'listing':
          setLoading(false)
          if (message.ok === false) {
            setError(message.message ?? t('files.error'))
          } else if (message.path && message.entries) {
            setListing({
              path: message.path,
              root: message.root ?? message.path,
              entries: message.entries,
            })
            setError(null)
          }
          break
        case 'sending': {
          const job = downloadRef.current
          // Only an answer to this panel's own outstanding request may be
          // adopted. Remote-clipboard staging uses the same channel.
          if (!job || !job.requesting || job.id !== 0) return
          job.requesting = false
          const size = message.size
          if (message.ok === false || !message.id || typeof size !== 'number' ||
              !Number.isSafeInteger(size) || size < 0) {
            finishDownload(job, message.message ?? t('files.error'))
            return
          }
          job.id = message.id
          job.size = size
          job.name = suggestedFileName(message.name ?? '', job.name)
          setError(null)
          armDownloadTimeout()
          setDownload({
            key: job.key,
            id: job.id,
            name: job.name,
            size: job.size,
            received: 0,
            phase: 'transferring',
          })
          connectionRef.current?.sendFileAction('pull', { id: job.id })
          break
        }
        case 'eof': {
          const job = downloadRef.current
          if (!job || (message.id && job.id !== message.id)) return
          if (message.ok === false) {
            finishDownload(job, message.message ?? t('files.error'))
            break
          }
          job.eofSeen = true
          // The final chunk's write may still be queued. Decide only after it
          // settles, so a short file is never committed.
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
        case 'cancelled':
          if (message.id && activeRef.current?.id === message.id) {
            finishActive('error', undefined, t('files.cancelled'))
          }
          if (message.id && downloadRef.current?.id === message.id) {
            const job = downloadRef.current
            if (job) finishDownload(job, t('files.cancelled'))
          }
          break
        case 'error':
          setError(message.message ?? t('files.error'))
          {
            const job = downloadRef.current
            if (job && (job.requesting || !message.id ||
                        message.id === job.id)) {
              finishDownload(job, message.message ?? t('files.error'))
              break
            }
          }
          if (message.id) finishActive('error', undefined, message.message)
          break
        default:
          break
      }
    },
    [armDownloadTimeout, finishActive, finishDownload, sendChunks, t],
  )

  useEffect(() => {
    if (!connection) return
    return connection.addListener((event) => {
      if (event.kind === 'clipboard-files') {
        setClipboardFiles(event.files)
        return
      }
      if (event.kind === 'file') {
        handleMessage(event.message)
        return
      }
      if (event.kind !== 'file-chunk') return
      const job = downloadRef.current
      if (!job || job.id !== event.id) return
      if (!acceptDownloadChunk(job, event.offset, event.data.byteLength)) {
        finishDownloadRef.current(job, t('files.incomplete'))
        return
      }
      if (job.writable) {
        // Stream straight to disk, keeping the stream writes ordered. The
        // next pull is sent only after the write settles, so a slow disk
        // does not queue an unbounded amount of data in memory.
        // Copy into a plain ArrayBuffer: the socket buffer may be reused, and
        // FileSystemWritableFileStream rejects a SharedArrayBuffer-backed view.
        const chunk = new Uint8Array(event.data).buffer
        job.writeChain = job.writeChain.then(async () => {
          if (job.cancelled) return
          try {
            await job.writable!.write(chunk)
          } catch (error) {
            job.writeError = error
          }
        })
      } else {
        job.chunks.push(event.data)
      }
      armDownloadTimeout()
      setDownload({
        key: job.key,
        id: job.id,
        name: job.name,
        size: job.size,
        received: job.received,
        phase: 'transferring',
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
        // The last chunk already sets received === size; pulling again would
        // hit a host transfer that has been erased.
        if (job.received < job.size) {
          connectionRef.current?.sendFileAction('pull', { id: job.id })
        }
      })
    })
  }, [armDownloadTimeout, connection, handleMessage, t])

  const browse = (path: string) => {
    setLoading(true)
    setError(null)
    connectionRef.current?.sendFileAction('browse', { path })
  }

  const selectReceive = () => {
    setTab('receive')
    connectionRef.current?.requestClipboardFiles()
    if (!listing) {
      setLoading(true)
      connectionRef.current?.sendFileAction('browse', { path: '' })
    }
  }

  // Stages what will be sent; "send" starts the queue. A folder drag arrives
  // as `sub/dir/file` paths so the host can recreate the tree.
  const enqueue = (entries: StagedFile[]) => {
    if (entries.length === 0) return
    for (const entry of entries) {
      const key = nextKey.current++
      const path = entry.path === '' ? entry.file.name : entry.path
      queueRef.current.push({ key, file: entry.file, path })
      setUploads((previous) => [
        ...previous,
        {
          key,
          name: path,
          size: entry.file.size,
          sent: 0,
          status: 'queued',
        },
      ])
    }
  }

  const onDrop = (event: DragEvent) => {
    event.preventDefault()
    // Keep the session surface from treating this as a drop-to-paste too.
    event.stopPropagation()
    if (event.dataTransfer) {
      void collectDropEntries(event.dataTransfer).then((items) =>
        enqueue(items),
      )
    }
  }

  // A drop on the session surface is handed to this drawer.
  useEffect(() => {
    return onDroppedFiles((items) => enqueue(items))
    // enqueue only touches refs and the upload list.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  if (!open) return null

  // Only an upload that is actually running blocks the button: counting the
  // queued items here disabled the button exactly when there was something to
  // send, so a transfer never started (it sat at 0 %).
  const busy = uploads.some((item) => item.status === 'sending')

  return (
    <div
      data-local-input="true"
      className="absolute inset-y-0 right-0 z-40 flex w-[min(94vw,380px)] flex-col border-l border-line bg-surface/95 shadow-2xl backdrop-blur-md"
      onPointerDown={(event) => event.stopPropagation()}
      onPointerUp={(event) => event.stopPropagation()}
    >
      <div className="flex h-11 shrink-0 items-center gap-2 border-b border-line-soft px-3">
        <FolderOpen size={15} className="text-accent" />
        <span className="text-[13px] font-medium">{t('files.title')}</span>
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

      <div className="flex shrink-0 gap-1 border-b border-line-soft p-2">
        <button
          type="button"
          onClick={() => setTab('send')}
          className={
            'flex flex-1 items-center justify-center gap-1.5 rounded-lg px-2 py-1.5 text-[11px] transition ' +
            (tab === 'send'
              ? 'bg-accent-soft text-accent'
              : 'text-muted hover:bg-panel hover:text-ink')
          }
        >
          <Upload size={13} />
          {t('files.send')}
        </button>
        <button
          type="button"
          onClick={selectReceive}
          className={
            'flex flex-1 items-center justify-center gap-1.5 rounded-lg px-2 py-1.5 text-[11px] transition ' +
            (tab === 'receive'
              ? 'bg-accent-soft text-accent'
              : 'text-muted hover:bg-panel hover:text-ink')
          }
        >
          <ArrowDownToLine size={13} />
          {t('files.receive')}
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

      {tab === 'send' ? (
        <div className="flex min-h-0 flex-1 flex-col gap-2 overflow-y-auto p-3">
          <div
            data-file-drop="true"
            onDragOver={(event) => {
              event.preventDefault()
              event.stopPropagation()
            }}
            onDrop={onDrop}
            className="rounded-xl border border-dashed border-line bg-panel/50 px-4 py-6 text-center"
          >
            <Upload size={20} className="mx-auto text-accent/70" />
            <div className="mt-2 text-[12px] text-ink">
              {t('files.dropHint')}
            </div>
            <div className="mt-1 text-[10px] text-muted">
              {t('files.receivePath')}
            </div>
            <button
              type="button"
              onClick={() => inputRef.current?.click()}
              className="btn btn-primary mt-3 h-8 px-3 text-[11px]"
            >
              {t('files.choose')}
            </button>
            <input
              ref={inputRef}
              type="file"
              multiple
              className="hidden"
              onChange={(event) => {
                const picked = event.target.files
                if (picked) {
                  enqueue(
                    Array.from(picked).map((file) => ({
                      file,
                      path: file.name,
                    })),
                  )
                }
                event.target.value = ''
              }}
            />
          </div>

          <div className="rounded-lg border border-line-soft bg-panel/50 p-2.5">
            <div className="text-[10px] uppercase tracking-[0.16em] text-muted">
              {t('files.destination')}
            </div>
            <div className="mt-1 flex items-center gap-2">
              <input
                value={dest}
                onChange={(event) => setDest(event.target.value)}
                placeholder="~/moLink"
                className="field flex-1 font-mono text-[11px]"
                aria-label={t('files.destination')}
              />
              <button
                type="button"
                disabled={busy || !uploads.some((item) => item.status === 'queued')}
                onClick={() => startNext()}
                className="btn btn-primary h-8 px-3 text-[11px]"
              >
                {t('files.send')}
              </button>
            </div>
            <div className="mt-1 text-[10px] leading-relaxed text-muted">
              {t('files.destinationHint')}
            </div>
          </div>

          <div className="space-y-1.5">
            {uploads.map((item) => {
              const percent =
                item.size === 0
                  ? item.status === 'done'
                    ? 100
                    : 0
                  : Math.min(100, Math.round((item.sent / item.size) * 100))
              return (
                <div
                  key={item.key}
                  className="rounded-lg border border-line-soft bg-panel/60 px-2.5 py-2"
                >
                  <div className="flex items-center gap-2">
                    <FileIcon size={13} className="shrink-0 text-muted" />
                    <span className="min-w-0 flex-1 truncate text-[11px] text-ink">
                      {item.name}
                    </span>
                    <span className="shrink-0 text-[10px] text-muted">
                      {formatBytes(item.size)}
                    </span>
                    <span className="shrink-0 text-[10px] text-muted">
                      {item.status === 'done'
                        ? t('files.done')
                        : item.status === 'error'
                          ? t('files.failed')
                          : `${percent}%`}
                    </span>
                  </div>
                  <div className="mt-1 h-1 overflow-hidden rounded-full bg-line-soft">
                    <div
                      className={
                        'h-full rounded-full transition-all ' +
                        (item.status === 'error' ? 'bg-danger' : 'bg-accent')
                      }
                      style={{ width: `${percent}%` }}
                    />
                  </div>
                  {item.path ? (
                    <div
                      className="mt-1 truncate font-mono text-[10px] text-muted"
                      title={item.path}
                    >
                      {item.path}
                    </div>
                  ) : null}
                  {item.error ? (
                    <div className="mt-1 text-[10px] text-danger">
                      {item.error}
                    </div>
                  ) : null}
                  {item.status === 'done' && item.path ? (
                    <button
                      type="button"
                      onClick={() =>
                        connectionRef.current?.sendFileAction(
                          'clipboard-set',
                          { paths: [item.path] },
                        )
                      }
                      className="btn mt-1.5 h-6 gap-1 px-2 text-[10px]"
                    >
                      <ClipboardPaste size={11} />
                      {t('files.copyToClipboard')}
                    </button>
                  ) : null}
                </div>
              )
            })}
          </div>
          {busy ? (
            <div className="text-center text-[10px] text-muted">
              {t('files.sending')}
            </div>
          ) : null}
        </div>
      ) : (
        <div className="flex min-h-0 flex-1 flex-col">
          <div className="flex shrink-0 items-center gap-1 border-b border-line-soft px-2 py-1.5">
            <button
              type="button"
              onClick={() => {
                if (listing) browse(parentPath(listing.path, listing.root))
              }}
              className="grid h-7 w-7 shrink-0 place-items-center rounded-md text-muted transition hover:bg-panel hover:text-ink"
              aria-label={t('files.up')}
              title={t('files.up')}
            >
              <ChevronLeft size={14} />
            </button>
            {listing ? (
              <button
                type="button"
                onClick={() => browse(listing.root)}
                className="grid h-7 w-7 shrink-0 place-items-center rounded-md text-muted transition hover:bg-panel hover:text-ink"
                aria-label={t('files.home')}
                title={t('files.home')}
              >
                <Home size={13} />
              </button>
            ) : null}
            {editingPath ? (
              <input
                ref={pathFieldRef}
                autoFocus
                value={pathDraft}
                spellCheck={false}
                onChange={(event) => setPathDraft(event.target.value)}
                onKeyDown={(event) => {
                  if (event.key === 'Enter') {
                    event.currentTarget.blur()
                  } else if (event.key === 'Escape') {
                    setEditingPath(false)
                  }
                }}
                onBlur={() => {
                  const draft = pathDraft.trim()
                  setEditingPath(false)
                  // An unknown path makes the host answer with an error and
                  // the listing stays where it was: the drawer simply returns
                  // to the previous folder.
                  if (draft !== '') browse(draft)
                }}
                className="field min-w-0 flex-1 font-mono text-[10px]"
                aria-label={t('files.path')}
              />
            ) : (
              <div
                className="flex min-w-0 flex-1 items-center gap-0.5 overflow-x-auto whitespace-nowrap font-mono text-[10px]"
                title={listing?.path ?? t('files.loading')}
                onDoubleClick={() => {
                  if (!listing) return
                  setPathDraft(listing.path)
                  setEditingPath(true)
                }}
              >
                {crumbs.map((crumb, index, all) => (
                    <span key={crumb.path} className="flex items-center gap-0.5">
                      {index > 0 ? (
                        <ChevronRight size={10} className="text-muted/60" />
                      ) : null}
                      <button
                        type="button"
                        onClick={() => browse(crumb.path)}
                        className={
                          'shrink-0 rounded px-0.5 transition hover:bg-panel ' +
                          (index === all.length - 1
                            ? 'text-ink'
                            : 'text-muted hover:text-accent')
                        }
                      >
                        {crumb.label}
                      </button>
                    </span>
                  ),
                )}
                {!listing ? t('files.loading') : null}
              </div>
            )}
            <button
              type="button"
              onClick={() => browse(listing?.path ?? '')}
              className="grid h-7 w-7 shrink-0 place-items-center rounded-md text-muted transition hover:bg-panel hover:text-ink"
              aria-label={t('files.refresh')}
              title={t('files.refresh')}
            >
              <RefreshCw size={12} />
            </button>
          </div>

          {clipboardFiles.length > 0 ? (
            <div className="mx-2 mt-2 rounded-xl border border-accent/30 bg-accent-soft/60 p-2.5">
              <div className="flex items-center gap-1.5">
                <ClipboardCopy size={12} className="text-accent" />
                <span className="text-[11px] font-medium text-accent">
                  {t('files.clipboardTitle')}
                </span>
                <span className="chip !px-1.5 !py-0 text-[9px]">
                  {clipboardFiles.length}
                </span>
                <button
                  type="button"
                  onClick={() => startDownloadAll(clipboardFiles)}
                  className="btn ml-auto h-6 px-2 text-[10px]"
                >
                  {t('files.receiveAll')}
                </button>
              </div>
              <div className="mt-1 text-[10px] leading-relaxed text-muted">
                {clipboardMode === 'local-app'
                  ? t('files.clipboardLocalApp')
                  : clipboardMode === 'auto'
                    ? t('files.clipboardAuto')
                    : t('files.clipboardHint')}
              </div>
              <ul className="mt-1.5 max-h-28 space-y-0.5 overflow-y-auto">
                {clipboardFiles.map((file) => (
                  <li key={file.path}>
                    <button
                      type="button"
                      onClick={() => startDownload(file.path, file.name)}
                      className="flex w-full items-center gap-2 rounded-md px-1.5 py-1 text-left transition hover:bg-panel/60"
                    >
                      <FileIcon size={12} className="shrink-0 text-muted" />
                      <span className="min-w-0 flex-1 truncate text-[11px] text-ink">
                        {file.name}
                      </span>
                      <span className="shrink-0 text-[9px] text-muted">
                        {formatSize(file.size)}
                      </span>
                    </button>
                  </li>
                ))}
              </ul>
            </div>
          ) : null}

          {download ? (
            <div className="mx-2 mt-2 rounded-lg border border-accent/35 bg-accent-soft px-2.5 py-2">
              <div className="flex items-center gap-2 text-[11px] text-ink">
                <ArrowDownToLine size={13} className="text-accent" />
                <span className="min-w-0 flex-1 truncate">
                  {download.name}
                </span>
                <span className="text-[10px] text-muted">
                  {download.phase === 'saved'
                    ? t('files.saved')
                    : download.phase === 'saving'
                      ? t('files.saving')
                      : `${formatSize(download.received)} / ${formatSize(download.size)}`}
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
            <div className="mx-2 mt-2 space-y-1">
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
                    {formatSize(item.size)}
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

          <div className="min-h-0 flex-1 overflow-y-auto p-2">
            {loading ? (
              <div className="py-8 text-center text-[11px] text-muted">
                {t('files.loading')}
              </div>
            ) : listing && listing.entries.length === 0 ? (
              <div className="py-8 text-center text-[11px] text-muted">
                {t('files.empty')}
              </div>
            ) : (
              <ul className="space-y-0.5">
                {listing?.entries.map((entry) => (
                  <li key={entry.name} className="group flex items-center gap-1">
                    <button
                      type="button"
                      onClick={() => {
                        if (!listing) return
                        const path = joinPath(listing.path, entry.name)
                        if (entry.dir) {
                          browse(path)
                        } else {
                          startDownload(path, entry.name)
                        }
                      }}
                      className="flex min-w-0 flex-1 items-center gap-2 rounded-lg px-2 py-1.5 text-left transition hover:bg-panel"
                    >
                      {entry.dir ? (
                        <Folder size={14} className="shrink-0 text-accent/80" />
                      ) : (
                        <FileIcon size={14} className="shrink-0 text-muted" />
                      )}
                      <span className="min-w-0 flex-1 truncate text-[12px] text-ink">
                        {entry.name}
                      </span>
                      <span className="shrink-0 text-[10px] text-muted">
                        {entry.dir ? '' : formatSize(entry.size)}
                      </span>
                    </button>
                    {entry.dir ? null : (
                      <button
                        type="button"
                        onClick={() =>
                          listing &&
                          startDownload(joinPath(listing.path, entry.name), entry.name)
                        }
                        className="grid h-7 w-7 shrink-0 place-items-center rounded-md text-muted opacity-0 transition hover:bg-panel hover:text-accent group-hover:opacity-100"
                        aria-label={t('files.download')}
                        title={t('files.download')}
                      >
                        <ArrowDownToLine size={13} />
                      </button>
                    )}
                  </li>
                ))}
              </ul>
            )}
          </div>
        </div>
      )}
    </div>
  )
}
