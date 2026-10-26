import { ApiError, fetchClipboardFile, fetchClipboardFiles } from './api'
import type { FileMessage } from './client'
import type { SessionConnection } from './session-connection'

interface QueuedFile {
  name: string
  size: number
  /// Clipboard files are read through the local core into memory (they are
  /// capped); dropped files stay as Blobs and are sliced while uploading.
  data?: ArrayBuffer
  file?: File
}

const MAX_CLIPBOARD_BYTES = 256 * 1024 * 1024
const MAX_DROP_BYTES = 2 * 1024 * 1024 * 1024
const RETRY_DELAY_MS = 3000

/// Mirrors files copied on this machine into the remote clipboard.
///
/// The page cannot read the OS clipboard's file list. The local core exposes
/// it over loopback instead; this class polls that snapshot, uploads new files
/// through the ordinary file-transfer channel, and asks the host to put the
/// uploaded paths on its clipboard (CF_HDROP / text/uri-list). A paste in the
/// remote desktop — including inside an RDP client with clipboard redirection
/// — then works without opening a panel or pressing a button.
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

  constructor(
    private readonly connection: () => SessionConnection | null,
    private readonly token: () => string,
    private readonly onError: (message: string) => void,
  ) {}

  /// Called once a second by the session. Cheap when nothing changed.
  async poll(): Promise<void> {
    if (this.busy || this.processing) return
    const connection = this.connection()
    const token = this.token()
    if (!connection || !token) return
    this.processing = true
    try {
      const result = await fetchClipboardFiles(token)
      const files = result.files.filter((file) => file.size > 0)
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
      const total = files.reduce((sum, file) => sum + file.size, 0)
      if (total > MAX_CLIPBOARD_BYTES) {
        this.onError('clipboard_files_too_large')
        this.lastKey = key
        return
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
      this.queue = queue
      this.paths = []
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
    }
  }

  /// Upload the files the user dropped on the remote screen, put them on the
  /// remote clipboard, and (when the session is controllable) send Ctrl+V so
  /// they land in the folder that has focus. The remote never sees a ghost
  /// entry during the drag — that needs a host filesystem agent — but the drop
  /// itself is a normal copy.
  async uploadFiles(files: File[], pasteAfterUpload: boolean): Promise<void> {
    if (this.busy || this.processing) {
      this.onError('clipboard_files_busy')
      return
    }
    if (files.length === 0) return
    const total = files.reduce((sum, file) => sum + file.size, 0)
    if (total > MAX_DROP_BYTES) {
      this.onError('clipboard_files_too_large')
      return
    }
    this.processing = true
    try {
      this.queue = files.map((file) => ({
        name: file.name,
        size: file.size,
        file,
      }))
      this.paths = []
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
        this.sendNext()
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
        this.sendNext()
        return
      }
    }
    if (message.action === 'clipboard') {
      if (message.ok === false) {
        this.finish('clipboard_files_failed')
      } else {
        if (this.pasteAfterDrop) this.pasteHere()
        this.finish(null)
      }
    }
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
    this.connection()?.sendFileAction('offer', {
      name: next.name,
      size: next.size,
    })
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
    // Abort the host-side upload too; otherwise its .part file lingers and
    // the next offer would reuse the same destination name.
    if (error && this.active) {
      this.connection()?.sendFileAction('cancel', { id: this.active.id })
    }
    if (error) this.onError(error)
    this.busy = false
    this.processing = false
    this.offerPending = false
    this.active = null
    this.queue = []
    this.paths = []
    this.pasteAfterDrop = false
    if (error) this.lastKey = ''
  }

  /// Drop an in-flight transfer when the session goes away.
  reset(): void {
    this.finish(null)
    this.lastKey = ''
  }
}
