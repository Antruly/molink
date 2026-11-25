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

/// Where a download's bytes actually land. The three destinations have very
/// different failure and resume behaviour, so the job logs and the partial-byte
/// bookkeeping both key off this instead of guessing from a container name.
export type DownloadSink = 'fs' | 'blob' | 'chunk'

export function downloadSinkFor(
  target: { stagingFile: boolean; writable: boolean },
): DownloadSink {
  // `chunk` is the local core's /api/fs/write staging file, `fs` is a File
  // System Access writable, and `blob` is the in-memory Save button fallback.
  if (target.stagingFile) return 'chunk'
  if (target.writable) return 'fs'
  return 'blob'
}

export type EofOutcome = 'complete' | 'waiting' | 'incomplete'

/// The host sends `eof` on the control channel, and that channel is drained
/// before file bytes (see channel_scheduler). The final chunk can therefore
/// arrive *after* the end marker. Keep waiting through a short grace window
/// instead of throwing away a transfer that is already complete on the wire.
export function eofOutcome(
  progress: DownloadProgress,
  eofAt: number,
  now: number,
  graceMs: number,
): EofOutcome {
  if (downloadComplete(progress)) return 'complete'
  if (progress.cancelled === true) return 'incomplete'
  if (progress.eofSeen !== true) return 'waiting'
  return now - eofAt >= graceMs ? 'incomplete' : 'waiting'
}

/// The exact `download.start` detail string. Names come from the host, so
/// keep the log line bounded and strip control characters that would break a
/// one-line export.
export function downloadStartDetail(
  sink: DownloadSink,
  name: string,
  size: number,
): string {
  const clean = name
    // eslint-disable-next-line no-control-regex
    .replace(/[\u0000-\u001f\u007f]/g, '?')
    .slice(0, 80)
  return `mode=${sink} name=${clean} size=${size}`
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
