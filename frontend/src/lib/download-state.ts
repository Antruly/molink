export interface DownloadProgress {
  size: number
  received: number
  eofSeen: boolean
  /// The viewer abandoned this transfer. Late chunks still in flight must not
  /// advance it (or complete it) after the cancel was sent.
  cancelled?: boolean
}

/// Marks a download as abandoned before its cancel reaches the host.
export function cancelDownload(progress: DownloadProgress): void {
  progress.cancelled = true
}

export function acceptDownloadChunk(
  progress: DownloadProgress, offset: number, length: number,
): boolean {
  if (progress.cancelled === true) return false
  if (offset !== progress.received || length <= 0 ||
      length > progress.size - progress.received) return false
  progress.received += length
  return true
}

export function downloadComplete(progress: DownloadProgress): boolean {
  return progress.cancelled !== true && progress.eofSeen &&
    progress.received === progress.size
}

/// The tiny slice of `localStorage` the resume bookkeeping needs. Keeping it
/// structural lets the tests use a Map and lets the panel pass a wrapper that
/// tolerates a disabled storage.
export interface OffsetStore {
  getItem(key: string): string | null
  setItem(key: string, value: string): void
  removeItem(key: string): void
}

/// Identity of one resumable download. The transfer id changes on every
/// reconnect, so the key is built from what survives: the host address, the
/// remote file's path (our file id), its size and mtime. A different file at
/// the same path gets a different key once it changes.
export function downloadResumeKey(
  host: string,
  fileId: string,
  size: number,
  mtime: number,
): string {
  return [
    'molink.download.v1',
    encodeURIComponent(host),
    encodeURIComponent(fileId),
    String(size),
    String(mtime),
  ].join('|')
}

function clampOffset(offset: number, size: number): number {
  if (!Number.isSafeInteger(offset) || offset < 0) return 0
  return offset > size ? size : offset
}

/// Records how many bytes of `key` are already on this machine. Zero is not
/// worth storing, so it clears the entry instead.
export function saveDownloadOffset(
  store: OffsetStore,
  key: string,
  offset: number,
  size: number,
): void {
  const value = clampOffset(offset, size)
  try {
    if (value <= 0 || value >= size) {
      store.removeItem(key)
      return
    }
    store.setItem(key, String(value))
  } catch {
    // Storage can be disabled (private mode, quota); the in-memory offset in
    // the active job still drives a reconnect.
  }
}

/// The saved offset for `key`, or 0 when there is none or it is not usable
/// for a file of `size` bytes.
export function loadDownloadOffset(
  store: OffsetStore,
  key: string,
  size: number,
): number {
  let raw: string | null = null
  try {
    raw = store.getItem(key)
  } catch {
    return 0
  }
  if (raw === null) return 0
  const value = Number(raw)
  if (!Number.isSafeInteger(value) || value <= 0 || value > size) return 0
  return value
}

export function clearDownloadOffset(store: OffsetStore, key: string): void {
  try {
    store.removeItem(key)
  } catch {
    // Nothing to clean up when storage is unavailable.
  }
}
