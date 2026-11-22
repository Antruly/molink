import { ApiError, fetchClipboardFile, fetchClipboardFiles } from './api'
import { ClipboardEcho } from './clipboard-echo'
import {
  LAZY_DEST,
  clipboardFileMode,
  directUploadEligible,
  resolveClipboardFetch,
  type LazyCapability,
} from './clipboard-lazy'
import type { FileMessage } from './client'
import type { SessionConnection } from './session-connection'

interface QueuedFile {
  name: string
  size: number
  /// The local core's opaque clipboard id, kept for the delayed path: the
  /// bytes are only read when the remote actually pastes.
  id?: string
  /// Clipboard files are read through the local core into memory (they are
  /// capped); dropped files stay as Blobs and are sliced while uploading.
  data?: ArrayBuffer
  file?: File
}

const RETRY_DELAY_MS = 3000
const LAZY_OFFER_TIMEOUT_MS = 10000

export interface ClipboardSyncOptions {
  /// Null when the host does not advertise `clipboard_lazy`.
  capability?: () => LazyCapability | null
  echo?: ClipboardEcho
}

/// Mirrors files copied on this machine into the remote clipboard.
///
/// The page cannot read the OS clipboard's file list. The local core exposes
/// it over loopback instead; this class polls that snapshot. When the host
/// supports delayed rendering it sends descriptors only (`clipboard-offer`)
/// and uploads a file when the host asks (`clipboard-fetch`); otherwise it
/// keeps the eager upload + `clipboard-set` path.
export class ClipboardFileSync {
  private busy = false
  private processing = false
  private lastKey = ''
  private lastAttempt = 0
  private queue: QueuedFile[] = []
  private offerPending = false
  private active: { id: number; file: QueuedFile } | null = null
  private paths: string[] = []
  private watchdog: number | null = null
  private pasteAfterDrop = false
  /// Which transfer the shared queue/active state belongs to.
  private mode: 'direct' | 'fetch' | null = null
  private fallbackNotified = false

  // Delayed clipboard state.
  private lazyFiles: QueuedFile[] = []
  private lazyDest = LAZY_DEST
  private lazyActive = false
  /// The host accepted a streaming offer: answer `fetch` ranges, never upload.
  private lazyStream = false
  private lazyOfferPending = false
  private lazyTag = 0
  private offerTimer: number | null = null
  private fetchQueue: { index: number; tag: number }[] = []
  private fetchActive: number | null = null
  /// Bumped when a new local selection replaces the offered one, so a fetch
  /// whose bytes were still loading cannot start an upload for the old list.
  private generation = 0

  private readonly echo: ClipboardEcho
  private readonly capability: () => LazyCapability | null

  constructor(
    private readonly connection: () => SessionConnection | null,
    private readonly token: () => string,
    private readonly onError: (message: string) => void,
    options: ClipboardSyncOptions = {},
  ) {
    this.echo = options.echo ?? new ClipboardEcho()
    this.capability = options.capability ?? (() => null)
  }

  /// Called once a second by the session. Cheap when nothing changed.
  async poll(): Promise<void> {
    if (this.busy || this.processing) return
    const connection = this.connection()
    const token = this.token()
    if (!connection || !token) return
    this.processing = true
    try {
      const result = await fetchClipboardFiles(token)
      // `echo` marks a file the local core just staged from the remote; the
      // reverse sync must not offer its own files back.
      const files = result.files.filter(
        (file) => file.size > 0 && file.echo !== true,
      )
      if (files.length === 0) {
        // Let the same files sync again after the clipboard was cleared.
        this.lastKey = ''
        return
      }
      const key = files
        .map((file) => `${file.id}:${file.size}`)
        .sort()
        .join('|')
      if (key === this.lastKey) return
      if (Date.now() - this.lastAttempt < RETRY_DELAY_MS) return
      this.lastAttempt = Date.now()

      const capability = this.capability()
      const mode = clipboardFileMode(files, capability)
      if (mode === 'stream' || mode === 'lazy') {
        // Descriptors only. In `stream` mode not a single byte is read here:
        // the host fetches ranges when (and where) the user pastes.
        if (mode === 'lazy' && !this.fallbackNotified) {
          // The host must materialise the file under its staging folder
          // before Explorer can copy it. Say so once, instead of letting the
          // user wonder why a shortcut copy appears there.
          this.fallbackNotified = true
          this.onError('clipboard_files_staged')
        }
        this.lastKey = key
        this.offer(files)
        return
      }
      // The host cannot delay, or the selection is outside its/our limits.
      if (mode === 'none') {
        this.onError('clipboard_files_too_large')
        this.lastKey = key
        return
      }
      if (capability === null && !this.fallbackNotified) {
        // One informational notice per session, not per copy.
        this.fallbackNotified = true
        this.onError('clipboard_files_uploaded')
      }
      const queue: QueuedFile[] = []
      for (const file of files) {
        queue.push({
          name: file.name,
          size: file.size,
          data: await fetchClipboardFile(token, file.id),
        })
      }
      this.lastKey = key
      this.dropLazyOffer()
      this.queue = queue
      this.paths = []
      this.mode = 'direct'
      this.busy = true
      this.sendNext()
    } catch (error) {
      // A host without a usable clipboard (headless, missing X11/Wayland
      // bridge) answers 403; a file that left the clipboard answers 404.
      // Neither is worth an error banner on every poll.
      if (
        error instanceof ApiError &&
        (error.status === 403 || error.status === 404)
      ) {
        return
      }
      this.onError('clipboard_files_failed')
    } finally {
      this.processing = false
      // A fetch may have arrived while the poll was reading the local
      // clipboard; nothing else would wake the queue.
      if (this.fetchQueue.length > 0) void this.pumpFetch()
    }
  }

  /// Upload the files the user dropped on the remote screen, put them on the
  /// remote clipboard, and (when the session is controllable) send Ctrl+V so
  /// they land in the folder that has focus. A drop is an explicit action, so
  /// it always uploads immediately instead of offering descriptors.
  async uploadFiles(files: File[], pasteAfterUpload: boolean): Promise<void> {
    if (this.busy || this.processing) {
      this.onError('clipboard_files_busy')
      return
    }
    if (files.length === 0) return
    if (!directUploadEligible(files)) {
      this.onError('clipboard_files_too_large')
      return
    }
    this.processing = true
    try {
      this.dropLazyOffer()
      this.queue = files.map((file) => ({
        name: file.name,
        size: file.size,
        file,
      }))
      this.paths = []
      this.mode = 'direct'
      this.pasteAfterDrop = pasteAfterUpload
      this.busy = true
      this.sendNext()
    } finally {
      this.processing = false
    }
  }

  /// The session's `file` events. Only this class's own transfer is consumed;
  /// the files panel has its own ids and is paused while it is open.
  handleMessage(message: FileMessage): void {
    // Delayed clipboard control messages arrive whether or not a transfer is
    // running.
    if (message.action === 'clipboard-offer') {
      this.handleOfferReply(message)
      return
    }
    if (message.action === 'fetch') {
      // Virtual file: one byte range of an offered file. Independent of the
      // upload queue, because Explorer reads several files concurrently.
      void this.answerFetch(message)
      return
    }
    if (message.action === 'clipboard-fetch') {
      const index = message.index
      // The host can only render an offer it accepted; anything else is a
      // stale fetch from a superseded selection.
      if (!this.lazyActive || typeof index !== 'number' ||
          !Number.isInteger(index) || index < 0 ||
          index >= this.lazyFiles.length) {
        this.connection()?.sendFileAction('clipboard-fetch-failed', {
          index: typeof index === 'number' ? index : 0,
        })
        return
      }
      this.fetchQueue.push({
        index,
        tag: typeof message.tag === 'number' ? message.tag : 0,
      })
      void this.pumpFetch()
      return
    }
    if (message.action === 'clipboard-rendered') {
      // The host materialised our offered files under these paths; the
      // reverse sync must skip them.
      this.echo.rememberPaths(message.paths ?? [])
      return
    }

    if (!this.busy) return
    if (message.action === 'accept' && this.offerPending && message.id) {
      this.clearWatchdog()
      this.offerPending = false
      const file = this.queue.shift()
      if (!file) {
        this.finish('clipboard_files_failed')
        return
      }
      this.active = { id: message.id, file }
      // The accept→done leg has no per-chunk acknowledgement; a stalled
      // transfer used to leave `busy` set forever and blocked every later
      // clipboard copy. The watchdog is re-armed as chunks go out.
      this.armWatchdog(30000)
      void this.sendChunks(
        message.id,
        file,
        message.chunk && message.chunk > 0 ? message.chunk : 256 * 1024,
      )
      return
    }
    if (message.action === 'done' && this.active &&
        message.id === this.active.id) {
      if (message.ok === false || !message.path) {
        // Skip the file that failed and keep the rest of the selection
        // moving; one unreadable file used to abort the whole copy.
        this.active = null
        if (this.mode === 'fetch') {
          this.fetchFailed()
          return
        }
        this.sendNext()
        return
      }
      if (this.mode === 'fetch') {
        this.active = null
        this.finishFetch()
        return
      }
      this.paths.push(message.path)
      this.active = null
      this.sendNext()
      return
    }
    if (message.action === 'reject' || message.action === 'cancel') {
      const activeMatch = this.active !== null &&
        message.id === this.active.id
      if (activeMatch || this.offerPending) {
        // Drop the failed item and continue the batch. An active transfer was
        // already shifted off the queue at accept; a rejected offer is still
        // its head.
        this.offerPending = false
        this.active = null
        if (!activeMatch) this.queue.shift()
        if (this.mode === 'fetch') {
          this.fetchFailed()
          return
        }
        this.sendNext()
        return
      }
    }
    if (message.action === 'clipboard') {
      if (message.ok === false) {
        this.finish('clipboard_files_failed')
      } else {
        // Remember the host paths this machine just wrote; the remote will
        // report them back and the reverse sync must not download them.
        this.echo.rememberPaths(this.paths)
        if (this.pasteAfterDrop) this.pasteHere()
        this.finish(null)
      }
    }
  }

  /// Offer descriptors without reading a single byte. The host registers them
  /// on its clipboard and fetches on the first paste.
  private offer(
    files: { id: string; name: string; size: number }[],
  ): void {
    // A newer local selection supersedes the previous offer. Avoid starting a
    // stale upload while the host is mid-render of the old list.
    this.generation += 1
    this.abortFetchTransfer()
    this.fetchQueue = []
    this.lazyFiles = files.map((file) => ({
      id: file.id,
      name: file.name,
      size: file.size,
    }))
    this.lazyDest = LAZY_DEST
    this.lazyActive = false
    this.lazyStream = false
    this.lazyOfferPending = true
    this.clearOfferTimer()
    this.connection()?.sendFileAction('clipboard-offer', {
      files: this.lazyFiles.map((file) => ({
        name: file.name,
        size: file.size,
      })),
      dest: this.lazyDest,
    })
    this.offerTimer = window.setTimeout(() => {
      this.offerTimer = null
      if (!this.lazyOfferPending) return
      this.lazyOfferPending = false
      this.lazyActive = false
      this.lazyStream = false
      this.lazyFiles = []
      this.lastKey = ''
      this.onError('clipboard_files_failed')
    }, LAZY_OFFER_TIMEOUT_MS)
    // The descriptors are ours; recognise them if they come back as files.
    this.echo.rememberFiles(this.lazyFiles)
  }

  private handleOfferReply(message: FileMessage): void {
    if (!this.lazyOfferPending) return
    this.clearOfferTimer()
    this.lazyOfferPending = false
    if (message.ok === false) {
      this.lazyActive = false
      this.lazyStream = false
      this.lazyFiles = []
      this.fetchQueue = []
      this.onError('clipboard_files_failed')
      return
    }
    this.lazyActive = true
    // The host streams only when it saw this viewer's `clipboard_stream`
    // hello flag; the flag in the reply is the authoritative answer.
    this.lazyStream = message.stream === true
  }

  /// Answer one host `fetch` with the requested range from this machine's
  /// clipboard. A failure is reported in-band (error code 1) so the host's
  /// waiting Read fails at once rather than waiting out its 64 KiB timeout.
  private async answerFetch(message: FileMessage): Promise<void> {
    const connection = this.connection()
    if (!connection) return
    const resolution = resolveClipboardFetch(message, this.lazyFiles)
    if (!resolution.ok) {
      if (resolution.id > 0) {
        connection.sendClipboardRange(
          resolution.id,
          resolution.error,
          resolution.offset,
          new Uint8Array(0),
        )
      }
      return
    }
    const { id, target } = resolution
    const failed = () => connection.sendClipboardRange(
      id, 1, target.offset, new Uint8Array(0),
    )
    if (!this.lazyActive || !this.lazyStream) {
      failed()
      return
    }
    let bytes: Uint8Array
    try {
      bytes = new Uint8Array(await fetchClipboardFile(
        this.token(),
        target.fileId,
        target.offset,
        target.length,
      ))
    } catch {
      // The file left the local clipboard between copy and paste.
      failed()
      return
    }
    // The host asked for exactly min(length, size-offset); a short reply is a
    // failed local read, not end of file, and must not truncate the paste.
    const expected = Math.min(target.length, target.size - target.offset)
    if (!this.lazyActive || !this.lazyStream ||
        bytes.byteLength !== expected) {
      failed()
      return
    }
    connection.sendClipboardRange(id, 0, target.offset, bytes)
  }

  /// One upload at a time, exactly like the eager path, but with the tag the
  /// host attached to its fetch so the bytes land in the right offer slot.
  private async pumpFetch(): Promise<void> {
    if (this.fetchActive !== null || this.busy || this.processing) return
    const next = this.fetchQueue.shift()
    if (!next) return
    const connection = this.connection()
    const token = this.token()
    const file = this.lazyFiles[next.index]
    if (!connection || !file || !file.id) {
      connection?.sendFileAction('clipboard-fetch-failed', {
        index: next.index,
      })
      return
    }
    const generation = this.generation
    this.processing = true
    let data: ArrayBuffer
    try {
      data = await fetchClipboardFile(token, file.id)
    } catch {
      this.processing = false
      // The local clipboard changed before the paste: fail this file at once
      // instead of letting the host wait out its 15 s budget.
      connection.sendFileAction('clipboard-fetch-failed', {
        index: next.index,
      })
      void this.pumpFetch()
      return
    }
    this.processing = false
    if (generation !== this.generation) {
      void this.pumpFetch()
      return
    }
    this.fetchActive = next.index
    this.lazyTag = next.tag
    this.mode = 'fetch'
    this.queue = [{ name: file.name, size: file.size, data }]
    this.paths = []
    this.pasteAfterDrop = false
    this.busy = true
    this.sendNext()
  }

  private fetchFailed(): void {
    const index = this.fetchActive
    if (index !== null) {
      this.connection()?.sendFileAction('clipboard-fetch-failed', { index })
    }
    this.finishFetch()
  }

  private finishFetch(): void {
    this.clearWatchdog()
    this.busy = false
    this.processing = false
    this.active = null
    this.queue = []
    this.paths = []
    this.fetchActive = null
    this.mode = null
    this.offerPending = false
    void this.pumpFetch()
  }

  /// Abandon an in-flight host fetch without touching the current lazy offer.
  private abortFetchTransfer(): void {
    if (this.mode !== 'fetch') return
    if (this.active) {
      this.connection()?.sendFileAction('cancel', { id: this.active.id })
    }
    if (this.fetchActive !== null) {
      this.connection()?.sendFileAction('clipboard-fetch-failed', {
        index: this.fetchActive,
      })
    }
    this.clearWatchdog()
    this.busy = false
    this.processing = false
    this.active = null
    this.queue = []
    this.fetchActive = null
    this.mode = null
    this.offerPending = false
  }

  private dropLazyOffer(): void {
    this.generation += 1
    this.abortFetchTransfer()
    this.fetchQueue = []
    this.lazyFiles = []
    this.lazyActive = false
    this.lazyStream = false
    this.lazyOfferPending = false
    this.clearOfferTimer()
  }

  /// Ctrl+V in the remote desktop after a drop. The remote application that
  /// has focus receives the files that were just put on its clipboard.
  private pasteHere(): void {
    this.connection()?.sendInput({
      events: [
        { k: 'key', code: 'ControlLeft', down: true },
        { k: 'key', code: 'KeyV', down: true },
        { k: 'key', code: 'KeyV', down: false },
        { k: 'key', code: 'ControlLeft', down: false },
      ],
    })
  }

  private sendNext(): void {
    if (this.queue.length === 0) {
      this.offerPending = false
      if (this.mode === 'fetch') {
        this.finishFetch()
        return
      }
      if (this.paths.length === 0) {
        this.finish('clipboard_files_failed')
        return
      }
      this.connection()?.sendFileAction('clipboard-set', {
        paths: this.paths,
      })
      this.armWatchdog()
      return
    }
    const next = this.queue[0]
    this.offerPending = true
    const extra: Record<string, unknown> = {
      name: next.name,
      size: next.size,
    }
    if (this.mode === 'fetch') {
      extra.dest = this.lazyDest
      extra.clipboard_tag = this.lazyTag
    }
    this.connection()?.sendFileAction('offer', extra)
    this.armWatchdog()
  }

  private armWatchdog(timeoutMs = 15000): void {
    if (this.watchdog !== null) window.clearTimeout(this.watchdog)
    this.watchdog = window.setTimeout(() => {
      this.watchdog = null
      if (this.busy) this.finish('clipboard_files_failed')
    }, timeoutMs)
  }

  private clearWatchdog(): void {
    if (this.watchdog !== null) {
      window.clearTimeout(this.watchdog)
      this.watchdog = null
    }
  }

  private clearOfferTimer(): void {
    if (this.offerTimer !== null) {
      window.clearTimeout(this.offerTimer)
      this.offerTimer = null
    }
  }

  private async sendChunks(
    id: number,
    file: QueuedFile,
    chunk: number,
  ): Promise<void> {
    let offset = 0
    let sent = 0
    this.armWatchdog(30000)
    while (offset < file.size) {
      const end = Math.min(file.size, offset + chunk)
      const bytes = file.data
        ? new Uint8Array(file.data, offset, end - offset)
        : new Uint8Array(await file.file!.slice(offset, end).arrayBuffer())
      this.connection()?.sendFileChunk(id, offset, bytes)
      offset = end
      sent += 1
      // Re-arm every few megabytes so a long but healthy transfer is not
      // mistaken for a stall, while a dead one still recovers.
      if (sent % 16 === 0) this.armWatchdog(30000)
      // Yield so a large file does not freeze the UI thread.
      await new Promise((resolve) => window.setTimeout(resolve, 0))
    }
    this.connection()?.sendFileAction('complete', { id })
    this.armWatchdog(30000)
  }

  private finish(error: string | null): void {
    this.clearWatchdog()
    if (error) {
      // Abort the host-side upload too; otherwise its .part file lingers and
      // the next offer would reuse the same destination name. The fetch path
      // also tells the host which slot failed so its render does not wait out
      // the 15 s budget.
      if (this.mode === 'fetch') {
        this.abortFetchTransfer()
      } else if (this.active) {
        this.connection()?.sendFileAction('cancel', { id: this.active.id })
      }
      this.onError(error)
      this.dropLazyOffer()
      this.lastKey = ''
    }
    this.busy = false
    this.processing = false
    this.offerPending = false
    this.active = null
    this.queue = []
    this.paths = []
    this.pasteAfterDrop = false
    this.fetchActive = null
    this.mode = null
    if (this.fetchQueue.length > 0) void this.pumpFetch()
  }

  /// Drop an in-flight transfer when the session goes away.
  reset(): void {
    this.finish(null)
    this.dropLazyOffer()
    this.lastKey = ''
  }
}
