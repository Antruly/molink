import {
  ApiError,
  appendClipboardStage,
  applyClipboardStage,
  stageClipboardFile,
} from './api'
import { stageableClipboardFile } from './clipboard-local'
import type { ClipboardFileEntry, FileMessage } from './client'
import {
  acceptDownloadChunk,
  downloadComplete,
  type DownloadProgress,
} from './download-state'
import type { SessionConnection } from './session-connection'

/// A host-side selection above this is left to the manual files panel. The
/// setting is off by default, so a user who enabled it opted into the traffic,
/// but a 4 GiB selection should still not start silently.
export const MAX_TOTAL_BYTES = 512 * 1024 * 1024
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

function failureCode(error: unknown): string {
  if (error instanceof ApiError && error.status === 413) {
    return 'remote_clipboard_too_large'
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

  constructor(
    private readonly connection: () => SessionConnection | null,
    private readonly token: () => string,
    private readonly onError: (code: string) => void,
    private readonly onApplied?: (count: number) => void,
  ) {}

  /// The settings switch. Turning it off abandons an in-flight batch; those
  /// staging entries expire lazily, and the host transfer is cancelled.
  setEnabled(enabled: boolean): void {
    this.enabled = enabled
    if (!enabled) this.cancel()
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
    if (!this.enabled || this.paused || this.busy || this.pendingCancel) return
    if (this.token() === '') return
    const usable = files.filter(
      (file) => file.path !== '' && stageableClipboardFile(file),
    )
    if (usable.length === 0) return
    const key = usable
      .map((file) => `${file.path}:${file.size}`)
      .sort()
      .join('|')
    if (key === this.lastKey) return
    const total = usable.reduce((sum, file) => sum + file.size, 0)
    if (total > MAX_TOTAL_BYTES ||
        usable.some((file) => file.size > MAX_FILE_BYTES)) {
      this.lastKey = key
      this.onError('remote_clipboard_too_large')
      return
    }
    this.lastKey = key
    this.queue = usable.slice()
    this.stagingIds = []
    this.busy = true
    this.startNext()
  }

  /// The session's `file` events. Returns true when this class owns the
  /// message, so SessionConnection does not forward it to the files panel
  /// (which would otherwise adopt a transfer it did not start).
  handleMessage(message: FileMessage): boolean {
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
  }

  reset(): void {
    this.cancel()
    this.pendingCancel = false
    this.lastKey = ''
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
  }
}
