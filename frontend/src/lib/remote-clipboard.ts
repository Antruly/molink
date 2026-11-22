import {
  ApiError,
  appendClipboardStage,
  applyClipboardStage,
  stageClipboardFile,
} from './api'
import { stageableClipboardFile } from './clipboard-local'
import { ClipboardEcho } from './clipboard-echo'
import {
  PULL_CONCURRENCY,
  checkPullChunk,
  clipboardBatchId,
  planPullFiles,
  pullChunkLength,
  type PullFile,
} from './clipboard-pull'
import type { ClipboardFileEntry, FileMessage } from './client'
import {
  acceptDownloadChunk,
  downloadComplete,
  type DownloadProgress,
} from './download-state'
import type { SessionConnection } from './session-connection'

/// Selection caps shared with the host's advisory limits. Files are streamed
/// to the staging folder, so the total is a traffic/policy cap, not a memory
/// one.
export const MAX_TOTAL_BYTES = 2 * 1024 * 1024 * 1024
export const MAX_FILE_BYTES = 256 * 1024 * 1024
const TRANSFER_TIMEOUT_MS = 30_000

interface ActiveFile extends DownloadProgress {
  entry: ClipboardFileEntry
  stageId: string
  id: number
  requesting: boolean
  finishing: boolean
  writeChain: Promise<void>
  writeError: unknown
  timer: number
}

/// One file being downloaded through the id-based pull path. Downloaded in
/// order, one outstanding range at a time; the host re-enumerates its
/// clipboard for every request.
interface ActivePull {
  entry: PullFile
  index: number
  stageId: string
  path: string
  offset: number
  received: number
  timer: number
  appending: boolean
}

interface PullItem {
  file: PullFile
  index: number
}

function failureCode(error: unknown): string {
  if (error instanceof ApiError && error.status === 413) {
    return 'remote_clipboard_too_large'
  }
  if (error instanceof ApiError && error.status === 503) {
    // This core has no way to write the OS clipboard (headless Linux, a
    // missing X11/Wayland bridge): say so instead of "transfer failed".
    return 'remote_clipboard_unavailable'
  }
  return 'remote_clipboard_failed'
}

/// Remote Explorer clipboard -> this machine's OS clipboard.
///
/// The host pushes `{"type":"clipboard","action":"files"}` with paths inside
/// its own home. This class reads those files through the ordinary
/// pull-driven file-transfer channel and writes each chunk to the *local*
/// core's staging area. Only after every file is complete does it call
/// `/api/clipboard-apply`, which creates the CF_HDROP / text/uri-list entry.
///
/// Only created when the page belongs to the local core; a remote browser has
/// no route to the staging APIs (loopback only) and must use the files panel.
export class RemoteClipboardSync {
  private queue: ClipboardFileEntry[] = []
  private active: ActiveFile | null = null
  private stagingIds: string[] = []
  private lastKey = ''
  private busy = false
  private paused = false
  private enabled = true
  /// A request was already on the wire when the batch was abandoned, so its
  /// `sending` answer has no id to cancel yet. Claim that answer and cancel
  /// it before the files panel (which shares the channel) can adopt it.
  private pendingCancel = false

  // Id-based pull path (clipboard-sync.files + clipboard-pull + 0x18).
  private pullActive = new Map<number, ActivePull>()
  private pullQueue: PullItem[] = []
  private pullStages: (string | undefined)[] = []
  private pullPaths: (string | undefined)[] = []
  private pullBatch = ''
  private pullLastKey = ''
  private pullStarting = false
  private pullCancelled = false
  /// An announcement that arrived while another transfer owned the shared
  /// channel. The host deduplicates announcements, so dropping it would lose
  /// that copy permanently.
  private pendingOffer: { files: unknown; omitted: number } | null = null
  private readonly usePull: () => boolean

  constructor(
    private readonly connection: () => SessionConnection | null,
    private readonly token: () => string,
    private readonly onError: (code: string) => void,
    private readonly onApplied?: (count: number) => void,
    private readonly echo?: ClipboardEcho,
    options: { usePull?: () => boolean } = {},
  ) {
    this.usePull = options.usePull ?? (() => true)
  }

  /// The settings switch. Turning it off abandons an in-flight batch; those
  /// staging entries expire lazily, and the host transfer is cancelled.
  setEnabled(enabled: boolean): void {
    this.enabled = enabled
    if (!enabled) this.cancel()
  }

  /// Whether the connected host uses the id-based pull path. The same host
  /// also sends the legacy path list for the files panel; when pull is
  /// available this class must not download that list a second time.
  usesPull(): boolean {
    return this.usePull()
  }

  /// The files drawer uses the same channel for manual downloads, so automatic
  /// staging pauses while it is open (and an in-flight batch is abandoned to
  /// keep the two consumers from racing over `sending`/`eof`).
  setPaused(paused: boolean): void {
    this.paused = paused
    if (paused) this.cancel()
  }

  /// Called for every `clipboard-files` event. `path` and `size` come from the
  /// host; nothing is staged until the user enabled the feature.
  handleFiles(files: ClipboardFileEntry[]): void {
    // A host that advertises clipboard_pull sends the same selection through
    // `clipboard-sync.files`; downloading both would duplicate the traffic.
    if (this.usePull()) return
    if (!this.enabled || this.paused || this.busy || this.pendingCancel) return
    if (this.token() === '') return
    const usable = files.filter(
      (file) => file.path !== '' && stageableClipboardFile(file),
    )
    if (usable.length === 0) return
    // Skip paths/descriptors this machine just wrote to the remote clipboard.
    // Without this the two directions stage each other's files forever
    // (`file (1)(2)`).
    const fresh = usable.filter(
      (file) =>
        this.echo?.suppressPath(file.path) !== true &&
        this.echo?.suppressFiles([file]) !== true,
    )
    if (fresh.length === 0) return
    const key = fresh
      .map((file) => `${file.path}:${file.size}`)
      .sort()
      .join('|')
    if (key === this.lastKey) return
    const total = fresh.reduce((sum, file) => sum + file.size, 0)
    if (total > MAX_TOTAL_BYTES ||
        fresh.some((file) => file.size > MAX_FILE_BYTES)) {
      this.lastKey = key
      this.onError('remote_clipboard_too_large')
      return
    }
    this.lastKey = key
    this.queue = fresh.slice()
    this.stagingIds = []
    this.busy = true
    this.startNext()
  }

  /// `clipboard-sync.files`: files copied on the host (possibly inside a
  /// nested RDP session). They are downloaded by id into the local core's
  /// staging folder and only become the OS clipboard once the whole set is
  /// complete -- a failed pull never leaves a half selection on the clipboard.
  handleOffer(rawFiles: unknown, hostOmitted = 0): void {
    if (!this.usePull()) return
    if (!this.enabled || this.paused || this.pendingCancel) return
    if (this.token() === '') return
    if (this.busy || this.pullActive.size > 0 || this.pullStarting) {
      this.pendingOffer = { files: rawFiles, omitted: hostOmitted }
      return
    }
    const plan = planPullFiles(rawFiles)
    if (plan.files.length === 0) {
      if (plan.omitted + hostOmitted > 0) {
        this.onError('remote_clipboard_truncated')
      }
      return
    }
    // Skip descriptors this machine just put on the remote clipboard.
    const fresh = plan.files.filter(
      (file) => this.echo?.suppressFiles([file]) !== true,
    )
    if (fresh.length === 0) return
    const key = fresh
      .map((file) => `${file.id}:${file.name}:${file.size}`)
      .join('|')
    if (key === this.pullLastKey) return
    this.pullLastKey = key
    if (plan.omitted + hostOmitted > 0) {
      this.onError('remote_clipboard_truncated')
    }
    this.busy = true
    this.pullCancelled = false
    this.pullStarting = true
    this.pullActive.clear()
    this.pullQueue = []
    this.pullStages = []
    this.pullPaths = []
    const nonce = `${Date.now().toString(36)}` +
      `${Math.random().toString(36).slice(2)}`
    void clipboardBatchId(fresh, nonce).then((batch) => {
      if (this.pullCancelled) return
      this.pullBatch = batch
      this.pullStages = new Array(fresh.length)
      this.pullPaths = new Array(fresh.length)
      this.pullQueue = fresh.map((file, index) => ({ file, index }))
      this.pullStarting = false
      this.pumpPull()
    }).catch(() => {
      this.pullStarting = false
      this.failPull('remote_clipboard_failed')
    })
  }

  /// One 0x18 range reply. Returns true when it belongs to this class.
  handlePullChunk(
    id: number,
    status: number,
    offset: number,
    data: Uint8Array,
  ): boolean {
    const active = this.pullActive.get(id)
    if (!active || active.stageId === '') return false
    if (active.appending) {
      // The host sent a second range before the first append settled.
      this.failPull('remote_clipboard_incomplete')
      return true
    }
    const expected = pullChunkLength(active.entry.size, active.offset)
    const check = checkPullChunk(
      status, offset, active.offset, expected, data.byteLength,
    )
    if (!check.ok) {
      this.failPull(
        check.reason === 'gone'
          ? 'remote_clipboard_gone'
          : check.reason === 'incomplete'
            ? 'remote_clipboard_incomplete'
            : 'remote_clipboard_failed',
      )
      return true
    }
    window.clearTimeout(active.timer)
    active.timer = 0
    active.appending = true
    // The event buffer belongs to the socket; copy before awaiting the write.
    const chunk = new Uint8Array(data)
    const at = active.offset
    const stageId = active.stageId
    appendClipboardStage(this.token(), stageId, at, chunk)
      .then(() => {
        if (this.pullCancelled) return
        active.appending = false
        active.offset += chunk.byteLength
        active.received += chunk.byteLength
        // Check the bytes, not the requested size: a zero-byte file never
        // sent a request and is completed in beginPull().
        if (active.offset >= active.entry.size) {
          this.finishPullFile(active)
        } else {
          this.requestPull(active)
        }
      })
      .catch((error) => {
        active.appending = false
        this.failPull(failureCode(error))
      })
    return true
  }

  private pumpPull(): void {
    if (!this.busy || this.pullCancelled) return
    while (
      this.pullActive.size < PULL_CONCURRENCY &&
      this.pullQueue.length > 0
    ) {
      const next = this.pullQueue.shift()
      if (!next) break
      void this.beginPull(next)
    }
    if (
      !this.pullStarting && this.pullActive.size === 0 &&
      this.pullQueue.length === 0
    ) {
      void this.applyPullAll()
    }
  }

  private async beginPull(item: PullItem): Promise<void> {
    const active: ActivePull = {
      entry: item.file,
      index: item.index,
      stageId: '',
      path: '',
      offset: 0,
      received: 0,
      timer: 0,
      appending: false,
    }
    // Reserve the concurrency slot before the awaited staging call.
    this.pullActive.set(item.file.id, active)
    try {
      const staged = await stageClipboardFile(
        this.token(),
        item.file.name,
        item.file.size,
        this.pullBatch,
      )
      if (this.pullCancelled) return
      active.stageId = staged.id
      active.path = staged.path
    } catch (error) {
      this.failPull(failureCode(error))
      return
    }
    this.pullStages[item.index] = active.stageId
    this.pullPaths[item.index] = active.path
    this.requestPull(active)
  }

  private requestPull(active: ActivePull): void {
    if (this.pullCancelled) return
    const length = pullChunkLength(active.entry.size, active.offset)
    if (length === 0) {
      this.finishPullFile(active)
      return
    }
    const connection = this.connection()
    if (connection === null) {
      this.failPull('remote_clipboard_failed')
      return
    }
    connection.sendFileAction('clipboard-pull', {
      id: active.entry.id,
      offset: active.offset,
      length,
    })
    window.clearTimeout(active.timer)
    active.timer = window.setTimeout(() => {
      if (this.pullActive.get(active.entry.id) === active) {
        this.failPull('remote_clipboard_failed')
      }
    }, TRANSFER_TIMEOUT_MS)
  }

  private finishPullFile(active: ActivePull): void {
    window.clearTimeout(active.timer)
    active.timer = 0
    this.pullActive.delete(active.entry.id)
    this.pumpPull()
  }

  private async applyPullAll(): Promise<void> {
    if (this.pullCancelled) return
    const ids = this.pullStages.filter(
      (id): id is string => typeof id === 'string' && id !== '',
    )
    if (ids.length !== this.pullStages.length || ids.length === 0) {
      this.failPull('remote_clipboard_incomplete')
      return
    }
    const paths = this.pullPaths.filter(
      (path): path is string => typeof path === 'string' && path !== '',
    )
    try {
      const result = await applyClipboardStage(this.token(), ids)
      if (this.pullCancelled) return
      // These are the paths the local core just put on the OS clipboard; the
      // local->remote poll must not offer them straight back.
      this.echo?.rememberPaths(paths)
      this.onApplied?.(result.count)
      this.busy = false
      this.pullStages = []
      this.pullPaths = []
      this.pullBatch = ''
      this.drainPendingOffer()
    } catch (error) {
      this.failPull(failureCode(error))
    }
  }

  /// Run an announcement that was deferred while another transfer was using
  /// the channel.
  private drainPendingOffer(): void {
    if (this.pendingOffer === null || this.busy) return
    const offer = this.pendingOffer
    this.pendingOffer = null
    this.handleOffer(offer.files, offer.omitted)
  }

  private failPull(code: string): void {
    this.pullCancelled = true
    for (const active of this.pullActive.values()) {
      window.clearTimeout(active.timer)
      active.timer = 0
    }
    this.pullActive.clear()
    this.pullQueue = []
    this.pullStages = []
    this.pullPaths = []
    this.pullStarting = false
    this.busy = false
    this.pullLastKey = ''
    this.onError(code)
    // A different announcement may have arrived during the failed batch; it
    // is still worth processing. The host deduplicates announcements, so the
    // same one cannot loop here.
    this.drainPendingOffer()
  }

  /// Stop the pull path without reporting an error (setting off, panel open,
  /// session gone).
  private cancelPull(): void {
    if (
      this.pullActive.size === 0 && this.pullQueue.length === 0 &&
      !this.pullStarting
    ) {
      return
    }
    this.pullCancelled = true
    for (const active of this.pullActive.values()) {
      window.clearTimeout(active.timer)
      active.timer = 0
    }
    this.pullActive.clear()
    this.pullQueue = []
    this.pullStages = []
    this.pullPaths = []
    this.pullStarting = false
    this.busy = false
    this.pullLastKey = ''
    this.pendingOffer = null
  }

  /// The session's `file` events. Returns true when this class owns the
  /// message, so SessionConnection does not forward it to the files panel
  /// (which would otherwise adopt a transfer it did not start).
  handleMessage(message: FileMessage): boolean {
    if (
      message.action === 'error' &&
      (this.pullActive.size > 0 || this.pullQueue.length > 0 ||
        this.pullStarting)
    ) {
      // The host rejected the pull (for example an older core that does not
      // know the action). Fail the batch instead of waiting out every timer.
      this.failPull('remote_clipboard_failed')
      return true
    }
    if (this.pendingCancel) {
      if (message.action === 'sending') {
        this.pendingCancel = false
        if (message.id) {
          this.connection()?.sendFileAction('cancel', { id: message.id })
        }
        return true
      }
      if (message.action === 'error') {
        this.pendingCancel = false
        return true
      }
    }
    const active = this.active
    if (!active) return false
    switch (message.action) {
      case 'sending': {
        if (!active.requesting || active.id !== 0) return false
        active.requesting = false
        if (message.ok === false || !message.id ||
            typeof message.size !== 'number' ||
            !Number.isSafeInteger(message.size) || message.size < 0) {
          this.fail('remote_clipboard_failed')
          return true
        }
        if (message.size !== active.size) {
          // The host file changed since the clipboard event; the staging
          // entry was declared with the old size and cannot accept it.
          this.fail('remote_clipboard_incomplete')
          return true
        }
        active.id = message.id
        this.armTimeout()
        this.connection()?.sendFileAction('pull', { id: active.id })
        return true
      }
      case 'eof': {
        if (message.id && active.id !== message.id) return false
        if (message.ok === false) {
          this.fail('remote_clipboard_incomplete')
          return true
        }
        active.eofSeen = true
        this.settle(active)
        return true
      }
      case 'error':
        if (!message.id || active.id === message.id) {
          this.fail('remote_clipboard_failed')
          return true
        }
        return false
      case 'cancelled':
        if (message.id && active.id === message.id) {
          this.fail('remote_clipboard_failed')
          return true
        }
        return false
      default:
        return false
    }
  }

  /// Binary frames arrive on a separate event path in SessionConnection.
  handleChunk(id: number, offset: number, data: Uint8Array): boolean {
    const active = this.active
    if (!active || active.id !== id) return false
    if (!acceptDownloadChunk(active, offset, data.byteLength)) {
      this.fail('remote_clipboard_incomplete')
      return true
    }
    // The event buffer belongs to the socket; copy before awaiting the HTTP
    // write so a reused buffer cannot corrupt the staged file.
    const chunk = new Uint8Array(data)
    const stageId = active.stageId
    active.writeChain = active.writeChain.then(async () => {
      if (active.cancelled) return
      try {
        await appendClipboardStage(this.token(), stageId, offset, chunk)
      } catch (error) {
        active.writeError = error
      }
    })
    this.armTimeout()
    this.settle(active)
    return true
  }

  /// Stop the current batch (panel opened, setting off, session gone).
  cancel(): void {
    const active = this.active
    if (active) {
      active.cancelled = true
      active.finishing = true
      window.clearTimeout(active.timer)
      if (active.id !== 0) {
        this.connection()?.sendFileAction('cancel', { id: active.id })
      } else if (active.requesting) {
        // The host has not answered `request` yet; claim its answer so the
        // files panel cannot mistake it for one of its own downloads.
        this.pendingCancel = true
      }
    }
    this.active = null
    this.queue = []
    this.stagingIds = []
    this.busy = false
    this.pendingOffer = null
    this.cancelPull()
  }

  reset(): void {
    this.cancel()
    this.pendingCancel = false
    this.lastKey = ''
    this.pullLastKey = ''
  }

  private startNext(): void {
    if (this.active) return
    const entry = this.queue.shift()
    if (!entry) {
      void this.applyAll()
      return
    }
    const active: ActiveFile = {
      entry,
      stageId: '',
      id: 0,
      size: entry.size,
      received: 0,
      eofSeen: false,
      cancelled: false,
      requesting: false,
      finishing: false,
      writeChain: Promise.resolve(),
      writeError: undefined,
      timer: 0,
    }
    this.active = active
    void this.begin(active)
  }

  private async begin(active: ActiveFile): Promise<void> {
    try {
      const staged = await stageClipboardFile(
        this.token(),
        active.entry.name,
        active.entry.size,
      )
      if (active.cancelled || this.active !== active) return
      active.stageId = staged.id
      this.stagingIds.push(staged.id)
    } catch (error) {
      this.fail(failureCode(error))
      return
    }
    if (active.cancelled || this.active !== active) return
    if (this.connection() === null) {
      this.fail('remote_clipboard_failed')
      return
    }
    active.requesting = true
    this.connection()?.sendFileAction('request', { path: active.entry.path })
    this.armTimeout()
  }

  /// After each chunk (and at EOF) decide whether the file is done. All
  /// decisions wait for the ordered HTTP appends to settle, so `eof` cannot
  /// get ahead of the last bytes.
  private settle(active: ActiveFile): void {
    void active.writeChain.then(() => {
      if (active.cancelled || active.finishing) return
      if (active.writeError !== undefined) {
        this.fail(failureCode(active.writeError))
        return
      }
      if (downloadComplete(active)) {
        active.finishing = true
        window.clearTimeout(active.timer)
        this.active = null
        this.startNext()
        return
      }
      if (active.eofSeen) {
        this.fail('remote_clipboard_incomplete')
        return
      }
      // The final chunk already brought `received` to `size`; pulling again
      // would address an erased host transfer.
      if (active.received < active.size) {
        this.connection()?.sendFileAction('pull', { id: active.id })
      }
    })
  }

  private armTimeout(): void {
    const active = this.active
    if (!active) return
    window.clearTimeout(active.timer)
    active.timer = window.setTimeout(() => {
      if (this.active === active) this.fail('remote_clipboard_failed')
    }, TRANSFER_TIMEOUT_MS)
  }

  private async applyAll(): Promise<void> {
    if (!this.busy) return
    if (this.stagingIds.length === 0) {
      this.busy = false
      return
    }
    const ids = this.stagingIds
    this.stagingIds = []
    try {
      const result = await applyClipboardStage(this.token(), ids)
      this.onApplied?.(result.count)
      this.busy = false
      this.drainPendingOffer()
    } catch (error) {
      this.busy = false
      this.lastKey = ''
      this.onError(failureCode(error))
    }
  }

  private fail(code: string): void {
    const active = this.active
    if (active) {
      active.cancelled = true
      active.finishing = true
      window.clearTimeout(active.timer)
      if (active.id !== 0) {
        this.connection()?.sendFileAction('cancel', { id: active.id })
      } else if (active.requesting) {
        this.pendingCancel = true
      }
    }
    this.active = null
    this.queue = []
    this.stagingIds = []
    this.busy = false
    this.lastKey = ''
    this.onError(code)
    this.drainPendingOffer()
  }
}
