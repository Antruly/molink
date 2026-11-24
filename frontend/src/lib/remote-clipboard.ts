import {
  ApiError,
  abortClipboardStage,
  appendClipboardStage,
  applyClipboardStage,
  clearVirtualClipboard,
  offerVirtualClipboard,
  stageClipboardFile,
} from './api'
import { stageableClipboardFile } from './clipboard-local'
import { ClipboardEcho } from './clipboard-echo'
import {
  PULL_CONCURRENCY,
  checkPullChunk,
  clipboardBatchId,
  mergeClipboardBatch,
  planExplicitPullFiles,
  planAutoPullFiles,
  planVirtualFiles,
  pullChunkLength,
  summarizePullFiles,
  type ClipboardBatchState,
  type PullFile,
} from './clipboard-pull'
import type { ClipboardFileEntry, FileMessage } from './client'
import {
  acceptDownloadChunk,
  downloadComplete,
  type DownloadProgress,
} from './download-state'
import type { SessionConnection } from './session-connection'
import { VirtualClipboardBridge } from './virtual-clipboard'

/// Selection caps shared with the host's advisory limits. Files are streamed
/// to the staging folder, so the total is a traffic/policy cap, not a memory
/// one.
export const MAX_TOTAL_BYTES = 2 * 1024 * 1024 * 1024
export const MAX_FILE_BYTES = 256 * 1024 * 1024
const TRANSFER_TIMEOUT_MS = 30_000

/// What the over-limit banner shows. `phase` drives the button: an idle offer
/// waits for an explicit click, `downloading` shows progress and a cancel
/// button, `failed` offers a retry.
export interface ClipboardOfferPrompt {
  count: number
  bytes: number
  /// Entries the host (or our own caps) left out of the offer.
  omitted: number
  phase: 'idle' | 'downloading' | 'failed'
  received: number
  /// Set when the virtual clipboard was expected but could not be used, so
  /// the banner can say the files are being downloaded instead.
  note?: 'virtual_unavailable'
}

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
  /// Accumulates a selection the host split into `clipboard-sync.files`
  /// batches. Null outside an in-flight batch.
  private offerBatch: ClipboardBatchState | null = null
  private offerBatchOmitted = 0
  private readonly usePull: () => boolean

  // Virtual clipboard path: the local core publishes the host's files (FUSE
  // mount on Linux, an OLE data object on Windows) and this class relays the
  // range reads. No bytes move on copy.
  private readonly useVirtual: () => boolean
  private readonly noteVirtualFallback: () => boolean
  private readonly onPrompt?: (prompt: ClipboardOfferPrompt | null) => void
  private virtual: VirtualClipboardBridge | null = null
  private virtualMount = ''
  private virtualStarting = false
  private virtualLastKey = ''
  /// Invalidates an in-flight offer/mount when the clipboard changes while the
  /// local core is still mounting.
  private virtualGeneration = 0
  /// The last over-limit selection, kept so the banner's button can fetch it.
  private explicitOffer: {
    files: PullFile[]
    omitted: number
  } | null = null
  private prompt: ClipboardOfferPrompt | null = null
  private pullReceived = 0
  private pullTotal = 0
  private lastProgressAt = 0
  /// Diagnostic only: the core stores it with the mount and logs it.
  private readonly sessionId =
    `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`

  constructor(
    private readonly connection: () => SessionConnection | null,
    private readonly token: () => string,
    private readonly onError: (code: string) => void,
    private readonly onApplied?: (count: number) => void,
    private readonly echo?: ClipboardEcho,
    options: {
      usePull?: () => boolean
      useVirtual?: () => boolean
      noteVirtualFallback?: () => boolean
      onPrompt?: (prompt: ClipboardOfferPrompt | null) => void
    } = {},
  ) {
    this.usePull = options.usePull ?? (() => true)
    this.useVirtual = options.useVirtual ?? (() => false)
    this.noteVirtualFallback = options.noteVirtualFallback ?? (() => false)
    this.onPrompt = options.onPrompt
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
    // A virtual mount does not use `sending`/`eof`, so opening the files panel
    // must not remove it. Only real downloads are abandoned.
    if (paused) {
      this.cancelDownloads()
      if (this.prompt?.phase === 'downloading') {
        this.explicitOffer = null
        this.setPrompt(null)
      }
    }
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
      (file) =>
        typeof file.path === 'string' && file.path !== '' &&
        stageableClipboardFile(file),
    )
    if (usable.length === 0) return
    // Skip paths/descriptors this machine just wrote to the remote clipboard.
    // Without this the two directions stage each other's files forever
    // (`file (1)(2)`).
    const fresh = usable.filter(
      (file) =>
        this.echo?.suppressPath(file.path ?? '') !== true &&
        this.echo?.suppressFiles([file]) !== true,
    )
    if (fresh.length === 0) return
    const key = fresh
      .map((file) => `${file.path ?? ''}:${file.size}`)
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
  /// nested RDP session).
  ///
  /// With a FUSE-capable local core the selection is mounted read-only and the
  /// OS clipboard names the mount; nothing is transferred until the local user
  /// pastes. Otherwise a small selection is downloaded into the staging folder,
  /// and a selection over the automatic caps is only offered behind an explicit
  /// button.
  handleOffer(
    rawFiles: unknown,
    hostOmitted = 0,
    batch?: { token?: unknown; index?: unknown; count?: unknown },
  ): void {
    if (!this.usePull()) return;
    // The host splits a large selection into 64-entry messages. Accumulate
    // them here: each piece alone is not the clipboard the user copied, and
    // the existing "a new selection cancels the old transfer" rule must only
    // run once the whole list is present.
    const merged = mergeClipboardBatch(
      this.offerBatch,
      batch ?? {},
      Array.isArray(rawFiles) ? rawFiles : [],
    );
    this.offerBatchOmitted = hostOmitted;
    if (!merged.ready) {
      this.offerBatch = merged.state;
      return;
    }
    this.offerBatch = null;
    const omitted = this.offerBatchOmitted;
    this.offerBatchOmitted = 0;
    this.processOffer(merged.files, omitted);
  }

  private processOffer(rawFiles: unknown, hostOmitted: number): void {
    if (!this.enabled || this.paused || this.pendingCancel) return
    if (this.token() === '') return
    const plan = planVirtualFiles(rawFiles)
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
      .map((file) => `${file.id}:${file.rel ?? file.name}:${file.size}:${file.isDir ? 1 : 0}`)
      .join('|')
    if (key === this.pullLastKey || key === this.virtualLastKey) return
    if (this.busy || this.pullActive.size > 0 || this.pullStarting ||
        this.virtualStarting) {
      // The remote clipboard changed while a transfer or mount was running.
      // That selection is no longer what the user copied: abandon it, delete
      // half-written staging files, and mount/download the new one.
      this.virtualGeneration += 1
      this.virtualStarting = false
      this.cancelDownloads()
      this.clearVirtual()
      this.explicitOffer = null
      this.setPrompt(null)
    }
    const forwardedOmitted = plan.omitted + hostOmitted

    if (this.useVirtual()) {
      void this.beginVirtual(fresh, forwardedOmitted)
      return
    }
    this.beginFallback(
      fresh,
      forwardedOmitted,
      this.noteVirtualFallback() ? 'virtual_unavailable' : undefined,
    )
  }

  /// Mount the selection with the local core (no bytes) and open the relay.
  private async beginVirtual(
    fresh: PullFile[],
    hostOmitted: number,
  ): Promise<void> {
    const key = fresh
      .map((file) => `${file.id}:${file.rel ?? file.name}:${file.size}:${file.isDir ? 1 : 0}`)
      .join('|')
    if (key === this.virtualLastKey && this.virtualMount !== '') return
    const generation = ++this.virtualGeneration
    this.virtualStarting = true
    this.busy = true
    let mount = ''
    try {
      const result = await offerVirtualClipboard(
        this.token(), fresh.map((file) => ({
          id: file.id,
          name: file.name,
          size: file.size,
          rel: file.rel,
          isDir: file.isDir,
        })), this.sessionId,
      )
      mount = result.mount
      if (generation !== this.virtualGeneration) {
        // The remote clipboard changed (or the session ended) while the core
        // was mounting. Do not leave a stale generation on the clipboard.
        void clearVirtualClipboard(this.token(), mount).catch(() => undefined)
        return
      }
      // The core already stopped the previous generation; drop the old relay.
      this.closeVirtualBridge()
      const bridge = new VirtualClipboardBridge(
        () => this.connection(),
        () => this.virtualClosed(),
      )
      await bridge.open(result.mount)
      if (generation !== this.virtualGeneration) {
        bridge.close()
        void clearVirtualClipboard(this.token(), mount).catch(() => undefined)
        return
      }
      this.virtual = bridge
      this.virtualMount = result.mount
      this.virtualLastKey = key
      this.virtualStarting = false
      this.busy = false
      this.setPrompt(null)
      if (hostOmitted + result.omitted > 0) {
        this.onError('remote_clipboard_truncated')
      }
    } catch (error) {
      if (mount !== '') {
        void clearVirtualClipboard(this.token(), mount).catch(() => undefined)
      }
      // A newer announcement owns the flags now; do not stomp them.
      if (generation !== this.virtualGeneration) return
      this.virtualStarting = false
      this.busy = false
      const availableFallback = this.isVirtualUnavailable(error)
      // The core cleared the old generation before failing, so the download
      // path is the only way to leave the user with something pasteable.
      this.beginFallback(
        fresh,
        hostOmitted,
        availableFallback ? 'virtual_unavailable' : undefined,
      )
      if (!availableFallback) this.onError('remote_clipboard_failed')
    }
  }

  private isVirtualUnavailable(error: unknown): boolean {
    if (error instanceof ApiError) {
      return (
        error.code === 'clipboard_virtual_unavailable' ||
        error.code === 'clipboard_virtual_mount_failed' ||
        error.code === 'clipboard_unavailable'
      )
    }
    return (
      error instanceof Error &&
      (error.message === 'clipboard_virtual_unavailable' ||
        error.message === 'clipboard_virtual_timeout')
    )
  }

  /// Download when the whole selection fits the automatic caps; otherwise
  /// leave it behind the banner's explicit button.
  private beginFallback(
    fresh: PullFile[],
    hostOmitted: number,
    note?: 'virtual_unavailable',
  ): void {
    const summary = summarizePullFiles(fresh)
    const auto = planAutoPullFiles(fresh)
    const over = auto.omitted > 0 || hostOmitted > 0
    if (over) {
      this.explicitOffer = { files: fresh, omitted: hostOmitted }
      this.setPrompt({
        count: summary.count,
        bytes: summary.bytes,
        omitted: auto.omitted + hostOmitted,
        phase: 'idle',
        received: 0,
        note,
      })
      return
    }
    this.startPull(fresh, hostOmitted, note)
  }

  /// The download path, shared by the automatic fallback and the explicit
  /// button. Runs in the background, one 256 KiB range at a time.
  private startPull(
    fresh: PullFile[],
    hostOmitted: number,
    note?: 'virtual_unavailable',
  ): void {
    const key = fresh
      .map((file) => `${file.id}:${file.rel ?? file.name}:${file.size}:${file.isDir ? 1 : 0}`)
      .join('|')
    if (key === this.pullLastKey) return
    this.pullLastKey = key
    // Keep the selection so a failed attempt can be retried from the banner.
    this.explicitOffer = { files: fresh.slice(), omitted: hostOmitted }
    if (hostOmitted > 0) {
      this.onError('remote_clipboard_truncated')
    }
    this.busy = true
    this.pullCancelled = false
    this.pullStarting = true
    this.pullActive.clear()
    this.pullQueue = []
    this.pullStages = []
    this.pullPaths = []
    this.pullReceived = 0
    this.pullTotal = fresh.reduce((sum, file) => sum + file.size, 0)
    // Every download, including the automatic fallback when FUSE is not
    // available, shows progress and a cancel action. Writing into the staging
    // folder silently and only surfacing a failure at paste time is what made
    // the file manager show its own "skip/cancel" dialog over a half copy.
    this.setPrompt({
      count: fresh.length,
      bytes: this.pullTotal,
      omitted: hostOmitted,
      phase: 'downloading',
      received: 0,
      note,
    })
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

  /// The banner's "取出" button: download the over-limit selection now.
  downloadOffer(): void {
    if (!this.enabled || this.explicitOffer === null) return
    if (this.busy || this.pullActive.size > 0 || this.pullStarting) return
    const plan = planExplicitPullFiles(this.explicitOffer.files)
    if (plan.files.length === 0) {
      this.onError('remote_clipboard_too_large')
      return
    }
    this.startPull(
      plan.files,
      this.explicitOffer.omitted + plan.omitted,
      this.isVirtualUnavailableNote() ? 'virtual_unavailable' : undefined,
    )
  }

  /// Stop an in-flight download but keep the over-limit offer so the button
  /// can retry it.
  cancelOffer(): void {
    this.cancelDownloads()
    if (this.explicitOffer !== null) {
      const summary = summarizePullFiles(this.explicitOffer.files)
      this.setPrompt({
        count: summary.count,
        bytes: summary.bytes,
        omitted: this.explicitOffer.omitted,
        phase: 'idle',
        received: 0,
        note: this.isVirtualUnavailableNote() ? 'virtual_unavailable' : undefined,
      })
    } else {
      this.setPrompt(null)
    }
  }

  /// Hide the banner (the files remain available through the files panel).
  dismissOffer(): void {
    this.cancelDownloads()
    this.explicitOffer = null
    this.setPrompt(null)
  }

  private isVirtualUnavailableNote(): boolean {
    return this.prompt?.note === 'virtual_unavailable'
  }

  private setPrompt(prompt: ClipboardOfferPrompt | null): void {
    this.prompt = prompt
    this.onPrompt?.(prompt)
  }

  private emitDownloadProgress(force = false): void {
    if (this.prompt === null || this.prompt.phase !== 'downloading') return
    const now = Date.now()
    if (!force && now - this.lastProgressAt < 200) return
    this.lastProgressAt = now
    this.setPrompt({ ...this.prompt, received: this.pullReceived })
  }

  /// The remote clipboard became text (or the session ended). The mount paths
  /// no longer describe the current clipboard, so unmount immediately.
  handleRemoteText(): void {
    // Invalidate a mount request that may still be in flight.
    this.virtualGeneration += 1
    this.virtualStarting = false
    this.cancelDownloads()
    if (this.virtual !== null || this.virtualMount !== '') {
      this.clearVirtual()
    }
    this.explicitOffer = null
    this.pullLastKey = ''
    this.setPrompt(null)
  }

  /// The user turned virtual paste off. Drop any mount immediately instead of
  /// waiting for the next clipboard change.
  disableVirtual(): void {
    this.virtualGeneration += 1
    this.virtualStarting = false
    this.clearVirtual()
  }

  private clearVirtual(): void {
    const mount = this.virtualMount
    this.virtualMount = ''
    this.virtualLastKey = ''
    this.closeVirtualBridge()
    if (mount !== '' && this.token() !== '') {
      void clearVirtualClipboard(this.token(), mount).catch(() => undefined)
    }
  }

  private closeVirtualBridge(): void {
    const bridge = this.virtual
    this.virtual = null
    bridge?.close()
  }

  private virtualClosed(): void {
    // The core sees the socket close and detaches the mount itself.
    this.virtual = null
    this.virtualMount = ''
    this.virtualLastKey = ''
  }

  /// One 0x18 range reply. Returns true when it belongs to this class.
  handlePullChunk(
    id: number,
    status: number,
    offset: number,
    data: Uint8Array,
  ): boolean {
    const active = this.pullActive.get(id)
    if (!active || active.stageId === '') {
      // No download owns this id: it is a virtual-file read.
      return this.virtual?.handleChunk(id, status, offset, data) === true
    }
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
        this.pullReceived += chunk.byteLength
        this.emitDownloadProgress()
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
        item.file.rel,
        item.file.isDir === true,
      )
      if (this.pullCancelled) {
        // The batch was cancelled while the staging entry was being created.
        // failPull() had already swept the ids it knew about; delete this late
        // one so a temp file never outlives the failed paste.
        void abortClipboardStage(this.token(), staged.id).catch(() => undefined)
        return
      }
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
      this.emitDownloadProgress(true)
      this.explicitOffer = null
      this.setPrompt(null)
      this.busy = false
      this.pullStages = []
      this.pullPaths = []
      this.pullBatch = ''
    } catch (error) {
      this.failPull(failureCode(error))
    }
  }


  private failPull(code: string): void {
    this.pullCancelled = true
    this.abortStaged(this.pullStages)
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
    if (this.prompt !== null) {
      this.setPrompt({
        ...this.prompt,
        phase: 'failed',
        received: this.pullReceived,
      })
    }
    this.onError(code)
    // A different announcement may have arrived during the failed batch; it
    // is still worth processing. The host deduplicates announcements, so the
    // same one cannot loop here.
  }

  /// Delete half-written staging files immediately. Entries otherwise expire
  /// lazily after 30 minutes, which is too late when the clipboard changed
  /// mid-download and the user can see the partial file in the cache.
  private abortStaged(ids: ReadonlyArray<string | undefined>): void {
    if (this.token() === '') return
    for (const id of ids) {
      if (typeof id === 'string' && id !== '') {
        void abortClipboardStage(this.token(), id).catch(() => undefined)
      }
    }
  }

  /// Abandon both download paths. A virtual mount does not share the
  /// `sending`/`eof` channel, so it survives the files panel opening.
  private cancelDownloads(): void {
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
    this.abortStaged(this.stagingIds)
    this.abortStaged(this.pullStages)
    this.active = null
    this.queue = []
    this.stagingIds = []
    this.busy = false
    this.lastKey = ''
    if (
      this.pullActive.size === 0 && this.pullQueue.length === 0 &&
      !this.pullStarting
    ) {
      return
    }
    this.pullCancelled = true
    for (const pull of this.pullActive.values()) {
      window.clearTimeout(pull.timer)
      pull.timer = 0
    }
    this.pullActive.clear()
    this.pullQueue = []
    this.pullStages = []
    this.pullPaths = []
    this.pullStarting = false
    this.pullLastKey = ''
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

  /// Stop everything (setting off, session gone). The virtual mount is
  /// released so its clipboard paths cannot outlive the transport.
  cancel(): void {
    this.virtualGeneration += 1
    this.virtualStarting = false
    this.cancelDownloads()
    this.clearVirtual()
    this.explicitOffer = null
    this.setPrompt(null)
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
    this.connection()?.sendFileAction('request', {
      path: active.entry.path ?? '',
    })
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
    } catch (error) {
      this.abortStaged(ids)
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
    this.abortStaged(this.stagingIds)
    this.active = null
    this.queue = []
    this.stagingIds = []
    this.busy = false
    this.lastKey = ''
    this.onError(code)
  }
}
