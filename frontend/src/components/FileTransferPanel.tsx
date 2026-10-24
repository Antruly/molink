import {
  ArrowDownToLine,
  ChevronLeft,
  ClipboardCopy,
  ClipboardPaste,
  File as FileIcon,
  Folder,
  FolderOpen,
  RefreshCw,
  Upload,
  X,
} from 'lucide-react'
import {
  useCallback,
  useEffect,
  useRef,
  useState,
  type DragEvent,
} from 'react'
import { useTranslation } from 'react-i18next'
import type {
  ClipboardFileEntry,
  FileEntry,
  FileMessage,
} from '../lib/client'
import type { SessionConnection } from '../lib/session-connection'

interface UploadItem {
  key: number
  name: string
  size: number
  sent: number
  status: 'queued' | 'sending' | 'done' | 'error'
  path?: string
  error?: string
}

interface DownloadItem {
  id: number
  name: string
  size: number
  received: number
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
  const [error, setError] = useState<string | null>(null)
  const [loading, setLoading] = useState(false)
  const [clipboardFiles, setClipboardFiles] = useState<ClipboardFileEntry[]>([])
  const [clipboardNote, setClipboardNote] = useState<string | null>(null)

  const queueRef = useRef<{ key: number; file: File }[]>([])
  const activeRef = useRef<{ key: number; file: File } | null>(null)
  const connectionRef = useRef<SessionConnection | null>(connection)
  const downloadRef = useRef<{
    id: number
    name: string
    size: number
    received: number
    chunks: Uint8Array[]
  } | null>(null)
  const nextKey = useRef(1)
  const lastPaint = useRef(0)
  const inputRef = useRef<HTMLInputElement>(null)
  const downloadQueue = useRef<string[]>([])

  const startDownload = useCallback((path: string) => {
    if (!path) return
    if (downloadRef.current) {
      downloadQueue.current.push(path)
      return
    }
    connectionRef.current?.sendFileAction('request', { path })
  }, [])

  const nextDownload = useCallback(() => {
    const next = downloadQueue.current.shift()
    if (next) {
      connectionRef.current?.sendFileAction('request', { path: next })
      return true
    }
    return false
  }, [])

  useEffect(() => {
    connectionRef.current = connection
    return () => {
      connectionRef.current = null
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
    connectionRef.current?.sendFileAction('offer', {
      name: next.file.name,
      size: next.file.size,
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
          if (!active || !message.id) return
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
          if (!message.id) return
          downloadRef.current = {
            id: message.id,
            name: message.name ?? 'file',
            size: message.size ?? 0,
            received: 0,
            chunks: [],
          }
          setDownload({
            id: message.id,
            name: message.name ?? 'file',
            size: message.size ?? 0,
            received: 0,
          })
          connectionRef.current?.sendFileAction('pull', { id: message.id })
          break
        }
        case 'eof': {
          const current = downloadRef.current
          if (!current || (message.id && current.id !== message.id)) return
          const blob = new Blob(current.chunks as BlobPart[])
          const url = URL.createObjectURL(blob)
          const anchor = document.createElement('a')
          anchor.href = url
          anchor.download = current.name
          document.body.appendChild(anchor)
          anchor.click()
          anchor.remove()
          window.setTimeout(() => URL.revokeObjectURL(url), 30_000)
          downloadRef.current = null
          setDownload(null)
          nextDownload()
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
          finishActive('error', undefined, t('files.cancelled'))
          break
        case 'error':
          setError(message.message ?? t('files.error'))
          if (
            downloadRef.current &&
            (!message.id || message.id === downloadRef.current.id)
          ) {
            downloadRef.current = null
            setDownload(null)
            nextDownload()
          }
          if (message.id) finishActive('error', undefined, message.message)
          break
        default:
          break
      }
    },
    [finishActive, nextDownload, sendChunks, t],
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
      const current = downloadRef.current
      if (!current || current.id !== event.id) return
      current.chunks.push(event.data)
      current.received += event.data.byteLength
      setDownload({
        id: current.id,
        name: current.name,
        size: current.size,
        received: current.received,
      })
      connectionRef.current?.sendFileAction('pull', { id: current.id })
    })
  }, [connection, handleMessage])

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

  const enqueue = (files: FileList | File[]) => {
    const items = Array.from(files)
    if (items.length === 0) return
    for (const file of items) {
      const key = nextKey.current++
      queueRef.current.push({ key, file })
      setUploads((previous) => [
        ...previous,
        {
          key,
          name: file.name,
          size: file.size,
          sent: 0,
          status: 'queued',
        },
      ])
    }
    startNext()
  }

  const onDrop = (event: DragEvent) => {
    event.preventDefault()
    enqueue(event.dataTransfer.files)
  }

  if (!open) return null

  const busy = uploads.some(
    (item) => item.status === 'queued' || item.status === 'sending',
  )

  return (
    <div
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
            onDragOver={(event) => event.preventDefault()}
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
                if (event.target.files) enqueue(event.target.files)
                event.target.value = ''
              }}
            />
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
              className="grid h-7 w-7 place-items-center rounded-md text-muted transition hover:bg-panel hover:text-ink"
              aria-label={t('files.up')}
              title={t('files.up')}
            >
              <ChevronLeft size={14} />
            </button>
            <div
              className="min-w-0 flex-1 truncate font-mono text-[10px] text-muted"
              title={listing?.path}
            >
              {listing?.path ?? t('files.loading')}
            </div>
            <button
              type="button"
              onClick={() => browse(listing?.path ?? '')}
              className="grid h-7 w-7 place-items-center rounded-md text-muted transition hover:bg-panel hover:text-ink"
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
                  onClick={() => {
                    for (const file of clipboardFiles) startDownload(file.path)
                  }}
                  className="btn ml-auto h-6 px-2 text-[10px]"
                >
                  {t('files.receiveAll')}
                </button>
              </div>
              <div className="mt-1 text-[10px] leading-relaxed text-muted">
                {t('files.clipboardHint')}
              </div>
              <ul className="mt-1.5 max-h-28 space-y-0.5 overflow-y-auto">
                {clipboardFiles.map((file) => (
                  <li key={file.path}>
                    <button
                      type="button"
                      onClick={() => startDownload(file.path)}
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
                  {formatSize(download.received)} / {formatSize(download.size)}
                </span>
              </div>
              <div className="mt-1 h-1 overflow-hidden rounded-full bg-line">
                <div
                  className="h-full rounded-full bg-accent transition-all"
                  style={{
                    width: `${
                      download.size > 0
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
                  <li key={entry.name}>
                    <button
                      type="button"
                      onClick={() => {
                        if (!listing) return
                        const path = joinPath(listing.path, entry.name)
                        if (entry.dir) {
                          browse(path)
                        } else {
                          startDownload(path)
                        }
                      }}
                      className="flex w-full items-center gap-2 rounded-lg px-2 py-1.5 text-left transition hover:bg-panel"
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
