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
