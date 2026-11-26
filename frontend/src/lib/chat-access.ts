/// Attachment byte access for the two chat surfaces.
///
/// The host dock can read a host path directly through the loopback `/api/fs`
/// bridge; a controlling viewer pulls it over the session file channel. Both
/// sides cache the resulting blob/object URL so a bubble that re-renders (or a
/// pane that is promoted to its own tab) does not download twice.
///
/// Downloads are serialised per panel: the file channel answers one `request`
/// at a time and identifies the reply only by name/size.

import { baseName, isAllowedAttachmentPath, type ChatAttachment } from './chat'
import {
  downloadChatFile,
  readHostFile,
  saveBlobAs,
} from './chat-transfer'
import type { SessionConnection } from './session-connection'

export interface ChatAttachmentAccess {
  /// A `data:` URL, a cached object URL, or null when no preview exists.
  thumbUrl(attachment: ChatAttachment): Promise<string | null>
  /// An object URL for the full file (image, video or save source).
  originalUrl(
    attachment: ChatAttachment,
    onProgress?: (received: number, total: number) => void,
  ): Promise<string | null>
  /// Ask for the bytes and hand them to the browser's save path.
  save(attachment: ChatAttachment): Promise<boolean>
  /// A file this client is sending right now: its bytes and preview are
  /// already in the page, so the bubble can render before the echo arrives.
  rememberLocal(id: string, file: File, thumb?: string | null): void
  dispose(): void
}

function isDataUrl(value: string): boolean {
  return value.startsWith('data:')
}

abstract class CachedAttachmentAccess implements ChatAttachmentAccess {
  private readonly thumbUrls = new Map<string, Promise<string | null>>()
  private readonly thumbBlobs = new Map<string, Promise<Blob | null>>()
  private readonly originalBlobs = new Map<string, Promise<Blob | null>>()
  private readonly originalUrls = new Map<string, Promise<string | null>>()
  private readonly localFiles = new Map<string, File>()
  private readonly localThumbs = new Map<string, string>()
  private readonly objectUrls = new Set<string>()
  private queue: Promise<unknown> = Promise.resolve()
  private disposed = false

  protected abstract fetchBlob(
    path: string,
    onProgress?: (received: number, total: number) => void,
    expectedSize?: number,
  ): Promise<Blob>

  rememberLocal(id: string, file: File, thumb?: string | null): void {
    if (id === '') return
    this.localFiles.set(id, file)
    if (thumb !== undefined && thumb !== null && thumb !== '') {
      this.localThumbs.set(id, thumb)
    }
  }

  /// React StrictMode runs an effect's cleanup and setup again on mount. The
  /// access object lives in a memo, so the second setup must be able to use it
  /// after the first cleanup revoked its object URLs.
  revive(): void {
    this.disposed = false
  }

  private serial<T>(task: () => Promise<T>): Promise<T> {
    const next = this.queue.then(task, task)
    this.queue = next.then(
      () => undefined,
      () => undefined,
    )
    return next
  }

  private track(url: string): string {
    this.objectUrls.add(url)
    return url
  }

  private localFile(attachment: ChatAttachment): File | null {
    return this.localFiles.get(attachment.id) ?? null
  }

  /// Fetch the thumbnail bytes. This runs inside the caller's `serial` slot:
  /// `thumbUrl` already holds the file channel, so queueing another task here
  /// would wait on itself and the preview would never resolve.
  private async fetchThumbBlob(
    attachment: ChatAttachment,
  ): Promise<Blob | null> {
    const path = attachment.thumb
    if (path === '' || isDataUrl(path)) return null
    const cached = this.thumbBlobs.get(path)
    if (cached) return cached
    const pending = (async () => {
      try {
        const blob = await this.fetchBlob(path)
        if (blob.size === 0) {
          // A missing/empty preview must not be remembered: a later render
          // (after a reconnect) should be allowed to try again.
          this.thumbBlobs.delete(path)
        }
        return blob
      } catch {
        this.thumbBlobs.delete(path)
        return null
      }
    })()
    this.thumbBlobs.set(path, pending)
    return pending
  }

  thumbUrl(attachment: ChatAttachment): Promise<string | null> {
    const local = this.localThumbs.get(attachment.id)
    if (local !== undefined) return Promise.resolve(local)
    if (isDataUrl(attachment.thumb)) return Promise.resolve(attachment.thumb)
    if (attachment.thumb === '') return Promise.resolve(null)
    const cached = this.thumbUrls.get(attachment.thumb)
    if (cached) return cached
    const pending = this.serial(async () => {
      const blob = await this.fetchThumbBlob(attachment)
      if (!blob || this.disposed) {
        this.thumbUrls.delete(attachment.thumb)
        return null
      }
      return this.track(URL.createObjectURL(blob))
    })
    this.thumbUrls.set(attachment.thumb, pending)
    return pending
  }

  /// Fetch the original bytes without queueing. Both public entry points take
  /// exactly one `serial` slot first: nesting one here would make the task
  /// wait for the slot it already holds (the file channel deadlocks).
  private async fetchOriginalBlob(
    attachment: ChatAttachment,
    onProgress?: (received: number, total: number) => void,
  ): Promise<Blob | null> {
    const local = this.localFile(attachment)
    if (local) return local
    const path = attachment.src
    if (path === '') return null
    const cached = this.originalBlobs.get(path)
    if (cached) return cached
    const pending = (async () => {
      try {
        return await this.fetchBlob(path, onProgress, attachment.size)
      } catch {
        // Keep no negative cache: the session may come back, and the user's
        // next Save click should be able to fetch the file again.
        this.originalBlobs.delete(path)
        return null
      }
    })()
    this.originalBlobs.set(path, pending)
    return pending
  }

  private originalBlob(
    attachment: ChatAttachment,
    onProgress?: (received: number, total: number) => void,
  ): Promise<Blob | null> {
    return this.serial(() =>
      this.fetchOriginalBlob(attachment, onProgress),
    )
  }

  originalUrl(
    attachment: ChatAttachment,
    onProgress?: (received: number, total: number) => void,
  ): Promise<string | null> {
    const local = this.localFile(attachment)
    if (local) {
      if (this.disposed) return Promise.resolve(null)
      return Promise.resolve(this.track(URL.createObjectURL(local)))
    }
    const key = attachment.src
    if (key === '') return Promise.resolve(null)
    const cached = this.originalUrls.get(key)
    if (cached) return cached
    const pending = this.serial(async () => {
      // Already inside the channel's slot: use the raw fetch, not
      // `originalBlob`, which would queue behind this very task.
      const blob = await this.fetchOriginalBlob(attachment, onProgress)
      if (!blob || this.disposed) {
        this.originalUrls.delete(key)
        return null
      }
      return this.track(URL.createObjectURL(blob))
    })
    this.originalUrls.set(key, pending)
    return pending
  }

  async save(attachment: ChatAttachment): Promise<boolean> {
    const blob = await this.originalBlob(attachment)
    if (!blob) return false
    return saveBlobAs(blob, attachment.name)
  }

  dispose(): void {
    this.disposed = true
    for (const url of this.objectUrls) URL.revokeObjectURL(url)
    this.objectUrls.clear()
    this.thumbUrls.clear()
    this.thumbBlobs.clear()
    this.originalBlobs.clear()
    this.originalUrls.clear()
    this.localFiles.clear()
    this.localThumbs.clear()
  }
}

/// The page served by the local core. Paths in attachment descriptors are
/// absolute host paths, which `/api/fs/read` accepts; they are only opened when
/// they resolve to the chat folders under the configured download root.
export class HostAttachmentAccess extends CachedAttachmentAccess {
  private readonly token: string
  private readonly root: string

  constructor(token: string, root: string) {
    super()
    this.token = token
    this.root = root
  }

  protected async fetchBlob(
    path: string,
    onProgress?: (received: number, total: number) => void,
  ): Promise<Blob> {
    if (this.token === '') throw new Error('no_token')
    if (!isAllowedAttachmentPath(this.root, path)) {
      throw new Error('outside_chat')
    }
    return readHostFile({
      token: this.token,
      path,
      ...(onProgress ? { onProgress } : {}),
    })
  }
}

/// A controlling viewer. The descriptor names a path on the host, which the
/// session file channel can pull.
export class RemoteAttachmentAccess extends CachedAttachmentAccess {
  private connection: SessionConnection | null

  constructor(connection: SessionConnection | null) {
    super()
    this.connection = connection
  }

  setConnection(connection: SessionConnection | null): void {
    this.connection = connection
  }

  protected async fetchBlob(
    path: string,
    onProgress?: (received: number, total: number) => void,
    expectedSize?: number,
  ): Promise<Blob> {
    if (!this.connection) throw new Error('offline')
    return downloadChatFile({
      connection: this.connection,
      path,
      expectedName: baseName(path),
      ...(expectedSize !== undefined ? { expectedSize } : {}),
      ...(onProgress ? { onProgress } : {}),
    })
  }
}
