export interface DownloadProgress {
  size: number
  received: number
  eofSeen: boolean
}

export function acceptDownloadChunk(
  progress: DownloadProgress, offset: number, length: number,
): boolean {
  if (offset !== progress.received || length <= 0 ||
      length > progress.size - progress.received) return false
  progress.received += length
  return true
}

export function downloadComplete(progress: DownloadProgress): boolean {
  return progress.eofSeen && progress.received === progress.size
}
