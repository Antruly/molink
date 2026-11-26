/// File-channel plumbing for chat attachments.
///
/// Uploads are ordinary `offer`/chunk/`complete` transfers. Sending the file
/// with the relative name `chat/<name>` makes the host create it underneath its
/// default destination, so the attachment lands in
/// `<download_root>/<remote>/<date>/chat/` without the viewer knowing that
/// root. Downloads use the same request/pull/eof path as the file panel.
///
/// The channel has a single `sending` reply per `request` and no client tag,
/// so callers serialise their downloads and match on name/size when they can.

import type { FileMessage, SessionEvent } from './client'
import { readLocalFileRange, writeLocalFileChunk } from './api'
import {
  basename,
  canUseFileSystemAccess,
  isPickerCancellation,
  suggestedFileName,
} from './download-save'
import type { SessionConnection } from './session-connection'

const FALLBACK_CHUNK = 256 * 1024
/// The host's receive window (16 chunks); staying below it avoids a nack.
const UPLOAD_WINDOW_BYTES = 16 * 256 * 1024
const OFFER_TIMEOUT_MS = 30_000
const ACK_TIMEOUT_MS = 20_000
const DONE_TIMEOUT_MS = 120_000
const DOWNLOAD_TIMEOUT_MS = 30_000
const HOST_READ_CHUNK = 4 * 1024 * 1024
const HOST_WRITE_CHUNK = 4 * 1024 * 1024
/// A peer can declare anything; do not let a hostile size fill the page's heap.
const MAX_DOWNLOAD_BYTES = 256 * 1024 * 1024
const MAX_HOST_READ_BYTES = 256 * 1024 * 1024

export interface UploadProgress {
  sent: number
  total: number
}

export interface ChatUploadResult {
  /// The final absolute path on the host.
  path: string
}

function abortedError(): Error {
  const error = new Error('aborted')
  error.name = 'AbortError'
  return error
}

function offlineError(): Error {
  return new Error('offline')
}

function messageOf(error: unknown, fallback: string): string {
  return error instanceof Error && error.message !== '' ? error.message : fallback
}

/// Resolve after `promise`, or reject when the timeout wins. The timer is
/// always cleared so a fast answer cannot hold the event loop open.
async function withTimeout<T>(
  promise: Promise<T>,
  timeoutMs: number,
  message: string,
): Promise<T> {
  let timer = 0
  try {
    return await Promise.race([
      promise,
      new Promise<never>((_, reject) => {
        timer = window.setTimeout(() => reject(new Error(message)), timeoutMs)
      }),
    ])
  } finally {
    window.clearTimeout(timer)
  }
}

interface Deferred<T> {
  promise: Promise<T>
  resolve: (value: T) => void
  reject: (error: unknown) => void
}

function deferred<T>(): Deferred<T> {
  let resolve!: (value: T) => void
  let reject!: (error: unknown) => void
  const promise = new Promise<T>((accept, deny) => {
    resolve = accept
    reject = deny
  })
  return { promise, resolve, reject }
}

function fileMessageOf(event: SessionEvent): FileMessage | null {
  return event.kind === 'file' ? event.message : null
}

/// Upload one blob through the session's file channel. `name` must be a safe
/// relative path such as `chat/report.docx`. Rejects on a host refusal, a lost
/// session, an abort or a timeout; the caller turns that into a failed bubble.
export async function uploadChatFile(options: {
  connection: SessionConnection
  name: string
  blob: Blob
  onProgress?: (progress: UploadProgress) => void
  signal?: AbortSignal
}): Promise<ChatUploadResult> {
  const { connection, name, blob, onProgress, signal } = options
  if (!connection.isLive()) throw offlineError()
  if (signal?.aborted) throw abortedError()

  let uploadId = 0
  let chunkSize = FALLBACK_CHUNK
  let confirmed = 0
  let accepted = false
  let finished = false
  let failure: Error | null = null
  let ackWake: (() => void) | null = null

  const acceptedDeferred = deferred<FileMessage>()
  const doneDeferred = deferred<string>()
  // A failure after the offer may reject either promise before the loop
  // reaches its await; mark them handled so the browser does not report an
  // unhandled rejection that the UI is already turning into a failed bubble.
  acceptedDeferred.promise.catch(() => {})
  doneDeferred.promise.catch(() => {})

  const wakeAck = () => {
    ackWake?.()
    ackWake = null
  }
  const fail = (error: Error) => {
    if (finished) return
    finished = true
    failure = error
    if (!accepted) acceptedDeferred.reject(error)
    doneDeferred.reject(error)
    wakeAck()
  }

  const listener = (event: SessionEvent) => {
    const message = fileMessageOf(event)
    if (!message) return
    switch (message.action) {
      case 'accept': {
        if (accepted || !message.id) return
        accepted = true
        uploadId = message.id
        chunkSize = message.chunk && message.chunk > 0
          ? message.chunk
          : FALLBACK_CHUNK
        confirmed = Math.min(message.offset ?? 0, blob.size)
        acceptedDeferred.resolve(message)
        return
      }
      case 'reject':
        fail(new Error(message.message ?? 'rejected'))
        return
      case 'error':
        if (message.id === undefined || message.id === uploadId ||
            !accepted) {
          fail(new Error(message.message ?? 'upload_failed'))
        }
        return
      case 'cancel':
        if (message.id === uploadId) {
          fail(new Error(message.message ?? 'cancelled'))
        }
        return
      case 'file-ack':
        if (message.id === uploadId) {
          const value = message.received ?? 0
          if (value > confirmed) confirmed = value
          wakeAck()
        }
        return
      case 'file-nack':
        if (message.id === uploadId) {
          confirmed = message.received ?? 0
          wakeAck()
        }
        return
      case 'done':
        if (message.id === uploadId) {
          if (message.ok === false) {
            fail(new Error(message.message ?? 'upload_failed'))
          } else {
            finished = true
            doneDeferred.resolve(message.path ?? '')
          }
        }
        return
      default:
        return
    }
  }

  const onAbort = () => {
    if (uploadId !== 0) {
      connection.sendFileAction('cancel', { id: uploadId })
    }
    fail(abortedError())
  }

  let unsubscribe: () => void = () => {}
  unsubscribe = connection.addListener(listener)
  signal?.addEventListener('abort', onAbort)
  let completed = false

  try {
    connection.sendFileAction('offer', { name, size: blob.size })
    const acceptedMessage = await withTimeout(
      acceptedDeferred.promise,
      OFFER_TIMEOUT_MS,
      'offer_timeout',
    )
    let offset = Math.min(acceptedMessage.offset ?? 0, blob.size)
    if (acceptedMessage.offset && acceptedMessage.offset > blob.size) {
      throw new Error('resume_offset_invalid')
    }
    confirmed = offset
    onProgress?.({ sent: offset, total: blob.size })

    while (offset < blob.size) {
      if (failure) throw failure
      if (signal?.aborted) throw abortedError()
      if (!connection.isLive()) throw offlineError()
      const end = Math.min(blob.size, offset + chunkSize)
      const bytes = new Uint8Array(
        await blob.slice(offset, end).arrayBuffer(),
      )
      if (failure) throw failure
      if (signal?.aborted) throw abortedError()
      connection.sendFileChunk(uploadId, offset, bytes)
      offset = end
      onProgress?.({ sent: offset, total: blob.size })

      if (offset - confirmed >= UPLOAD_WINDOW_BYTES) {
        const reached = await new Promise<boolean>((resolve) => {
          const timer = window.setTimeout(() => {
            ackWake = null
            resolve(false)
          }, ACK_TIMEOUT_MS)
          ackWake = () => {
            window.clearTimeout(timer)
            resolve(true)
          }
        })
        if (failure) throw failure
        if (!reached) {
          // The host stopped acknowledging. Rewind and replay; the file
          // channel treats a duplicate range as idempotent.
          offset = Math.min(confirmed, blob.size)
          continue
        }
        if (signal?.aborted) throw abortedError()
      }
      // Let the page paint progress even for a fast local link.
      await new Promise((resolve) => window.setTimeout(resolve, 0))
    }

    if (failure) throw failure
    if (!connection.isLive()) throw offlineError()
    connection.sendFileAction('complete', { id: uploadId })
    const path = await withTimeout(
      doneDeferred.promise,
      DONE_TIMEOUT_MS,
      'upload_timeout',
    )
    if (path === '') throw new Error('upload_path_missing')
    completed = true
    return { path }
  } finally {
    if (!completed && uploadId !== 0 && connection.isLive()) {
      // Release the host's upload slot/file immediately when the page gave up
      // (or a chunk send failed) instead of waiting for its idle reclaim.
      connection.sendFileAction('cancel', { id: uploadId })
    }
    finished = true
    unsubscribe()
    signal?.removeEventListener('abort', onAbort)
  }
}

function sameName(left: string, right: string): boolean {
  return left === right || left.toLowerCase() === right.toLowerCase()
}

/// Pull one host file into memory. Used for thumbnails and for opening an
/// attachment. `expectedName`/`expectedSize` let this consumer ignore a
/// `sending` answer that belongs to the file panel or the clipboard sync.
export async function downloadChatFile(options: {
  connection: SessionConnection
  path: string
  expectedName?: string
  expectedSize?: number
  onProgress?: (received: number, total: number) => void
  signal?: AbortSignal
}): Promise<Blob> {
  const { connection, path, onProgress, signal } = options
  if (!connection.isLive()) throw offlineError()
  if (signal?.aborted) throw abortedError()

  const result = deferred<Blob>()
  let downloadId = 0
  let size = -1
  let received = 0
  let chunks: Uint8Array[] = []
  let timer = 0
  let settled = false
  let unsubscribe: () => void = () => {}

  const cleanup = () => {
    window.clearTimeout(timer)
    unsubscribe()
    signal?.removeEventListener('abort', onAbort)
  }
  const finish = (error: Error | null) => {
    if (settled) return
    settled = true
    cleanup()
    if (error) {
      result.reject(error)
    } else {
      result.resolve(new Blob(chunks as BlobPart[]))
    }
  }
  const arm = () => {
    window.clearTimeout(timer)
    timer = window.setTimeout(
      () => finish(new Error('download_timeout')),
      DOWNLOAD_TIMEOUT_MS,
    )
  }
  const onAbort = () => {
    if (downloadId !== 0) connection.sendFileAction('cancel', { id: downloadId })
    finish(abortedError())
  }

  const listener = (event: SessionEvent) => {
    if (event.kind === 'file-chunk') {
      if (downloadId === 0 || event.id !== downloadId) return
      if (event.offset !== received) {
        finish(new Error('download_out_of_order'))
        return
      }
      if (received + event.data.byteLength > size) {
        finish(new Error('download_overflow'))
        return
      }
      // The socket buffer is reused after the handler returns.
      chunks.push(new Uint8Array(event.data))
      received += event.data.byteLength
      arm()
      onProgress?.(received, size)
      if (received < size) {
        connection.sendFileAction('pull', { id: downloadId })
      }
      return
    }
    const message = fileMessageOf(event)
    if (!message) return
    if (message.action === 'sending') {
      if (downloadId !== 0 || settled) return
      if (message.ok === false || !message.id ||
          typeof message.size !== 'number' ||
          !Number.isSafeInteger(message.size) || message.size < 0) {
        finish(new Error(message.message ?? 'download_rejected'))
        return
      }
      if (options.expectedName !== undefined &&
          !sameName(message.name ?? '', options.expectedName)) {
        return
      }
      if (options.expectedSize !== undefined &&
          message.size !== options.expectedSize) {
        return
      }
      if (message.size > MAX_DOWNLOAD_BYTES) {
        finish(new Error('download_too_large'))
        return
      }
      downloadId = message.id
      size = message.size
      received = 0
      chunks = []
      arm()
      onProgress?.(0, size)
      connection.sendFileAction('pull', { id: downloadId })
      return
    }
    if (message.action === 'eof') {
      if (downloadId === 0 || (message.id !== undefined && message.id !== downloadId)) {
        return
      }
      if (message.ok === false) {
        finish(new Error(message.message ?? 'download_failed'))
        return
      }
      if (received !== size) {
        finish(new Error('download_incomplete'))
        return
      }
      finish(null)
      return
    }
    if (message.action === 'error' || message.action === 'cancel' ||
        message.action === 'cancelled') {
      if (downloadId === 0 ||
          message.id === undefined || message.id === downloadId) {
        finish(new Error(message.message ?? 'download_failed'))
      }
    }
  }

  unsubscribe = connection.addListener(listener)
  signal?.addEventListener('abort', onAbort)
  arm()
  try {
    connection.sendFileAction('request', { path })
    return await result.promise
  } finally {
    if (!settled) {
      settled = true
      cleanup()
    }
  }
}

/// Read a host-local file through the loopback `/api/fs` bridge. Only the page
/// served by the core at the machine itself may do this; it is how the host
/// dock opens attachments that the remote uploaded.
export async function readHostFile(options: {
  token: string
  path: string
  expectedSize?: number
  onProgress?: (received: number, total: number) => void
  signal?: AbortSignal
}): Promise<Blob> {
  const { token, path, expectedSize, onProgress, signal } = options
  const chunks: BlobPart[] = []
  let offset = 0
  while (offset <= MAX_HOST_READ_BYTES) {
    if (signal?.aborted) throw abortedError()
    const bytes = await readLocalFileRange(
      token,
      path,
      offset,
      HOST_READ_CHUNK,
    )
    if (bytes.byteLength === 0) break
    chunks.push(new Uint8Array(bytes))
    offset += bytes.byteLength
    onProgress?.(offset, expectedSize ?? 0)
  }
  if (offset > MAX_HOST_READ_BYTES) throw new Error('file_too_large')
  return new Blob(chunks)
}

/// Write a blob into the host's attachment folder in 4 MiB pieces. The first
/// piece truncates so a retry replaces a half-written file.
export async function writeHostFile(options: {
  token: string
  path: string
  blob: Blob
  onProgress?: (sent: number, total: number) => void
  signal?: AbortSignal
}): Promise<void> {
  const { token, path, blob, onProgress, signal } = options
  let offset = 0
  do {
    if (signal?.aborted) throw abortedError()
    const end = Math.min(blob.size, offset + HOST_WRITE_CHUNK)
    const bytes = new Uint8Array(await blob.slice(offset, end).arrayBuffer())
    await writeLocalFileChunk(token, path, offset, bytes, offset === 0)
    offset = end
    onProgress?.(offset, blob.size)
  } while (offset < blob.size)
}

/// Save a downloaded/read attachment. The picker path streams straight to the
/// user's chosen file; browsers without the File System Access API fall back
/// to an `<a download>` click, which has user activation inside the handler.
export async function saveBlobAs(blob: Blob, name: string): Promise<boolean> {
  const suggested = suggestedFileName(name)
  if (canUseFileSystemAccess(window)) {
    try {
      const handle = await window.showSaveFilePicker!({ suggestedName: suggested })
      const writable = await handle.createWritable()
      await writable.write(blob)
      await writable.close()
      return true
    } catch (error) {
      if (isPickerCancellation(error)) return false
      // Fall through to the anchor: a picker that exists but refuses (an
      // opaque origin, a browser policy) must not lose the bytes.
    }
  }
  const url = URL.createObjectURL(blob)
  try {
    const anchor = document.createElement('a')
    anchor.href = url
    anchor.download = suggested
    anchor.rel = 'noopener'
    document.body.appendChild(anchor)
    anchor.click()
    anchor.remove()
    return true
  } finally {
    window.setTimeout(() => URL.revokeObjectURL(url), 30_000)
  }
}

export function fileNameOf(path: string): string {
  return basename(path)
}

export function uploadErrorMessage(error: unknown): string {
  return messageOf(error, 'upload_failed')
}
