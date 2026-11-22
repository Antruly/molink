import type { DownloadProgress } from './download-state'

/// How a completed download reaches the disk.
///
/// `picker` streams the chunks into a FileSystemWritableFileStream opened from
/// a user gesture (File System Access API). `button` buffers the file in memory
/// and shows a Save button, because calling `anchor.click()` from the WebSocket
/// callback has no user activation and Chrome files it as an unconfirmed
/// automatic download (the `.crdownload` left in a temp folder).
export type SaveStrategy = 'picker' | 'button'

export interface FileSystemAccessScope {
  isSecureContext?: boolean
  showSaveFilePicker?: unknown
  showDirectoryPicker?: unknown
}

/// The File System Access API is only exposed on secure contexts. A plain
/// `http://192.168.x.x` page therefore must fall back to the Save button, even
/// if a browser otherwise implements the API.
export function canUseFileSystemAccess(scope: FileSystemAccessScope): boolean {
  return scope.isSecureContext === true &&
    typeof scope.showSaveFilePicker === 'function'
}

export function canPickDirectory(scope: FileSystemAccessScope): boolean {
  // The File System Access API is secure-context only; on a plain
  // `http://192.168.x.x` page `showDirectoryPicker` either is absent or throws
  // SecurityError when called. Check both so the caller falls back to the
  // `webkitdirectory` input instead of opening a picker that cannot work.
  return scope.isSecureContext === true &&
    typeof scope.showDirectoryPicker === 'function'
}

/// Whether a hidden `<input type="file">` can be made into a folder picker.
/// `webkitdirectory` is not standard, so a browser that ignores it silently
/// falls back to a plain file picker; callers should warn in that case.
export interface DirectoryInputScope {
  createElement?: (tag: string) => object
}

export function supportsDirectoryInput(scope: DirectoryInputScope): boolean {
  try {
    if (typeof scope.createElement !== 'function') return false
    return 'webkitdirectory' in scope.createElement('input')
  } catch {
    return false
  }
}

export function saveStrategyFor(scope: FileSystemAccessScope): SaveStrategy {
  return canUseFileSystemAccess(scope) ? 'picker' : 'button'
}

/// Cancelling the native picker is not a transfer failure: no bytes were
/// requested, so the queue must move on without an error banner.
export function isPickerCancellation(error: unknown): boolean {
  return typeof error === 'object' && error !== null &&
    (error as { name?: unknown }).name === 'AbortError'
}

/// The Save button is only meaningful once the whole file arrived (same
/// `eofSeen && received === size` rule as every other download path) and the
/// transfer was not abandoned.
export function maySaveFromButton(
  strategy: SaveStrategy,
  progress: DownloadProgress,
  failed: boolean,
): boolean {
  return strategy === 'button' && !failed &&
    progress.cancelled !== true && progress.eofSeen &&
    progress.received === progress.size
}

/// A streamed download owns a writable with a partially written temp file.
/// Every failure path must abort it, or Chrome commits a half file when the
/// stream is closed by garbage collection.
export function mustAbortStream(
  strategy: SaveStrategy,
  committed: boolean,
): boolean {
  return strategy === 'picker' && !committed
}

export function basename(path: string): string {
  const text = path.replace(/[\\/]+$/, '')
  const cut = Math.max(text.lastIndexOf('/'), text.lastIndexOf('\\'))
  return cut >= 0 ? text.slice(cut + 1) : text
}

/// Filenames reported by the host can contain characters the picker rejects.
/// Keep the suggested name harmless without dropping the extension.
export function suggestedFileName(name: string, fallbackPath: string): string {
  const raw = name.trim() !== '' ? name : basename(fallbackPath)
  const cleaned = raw.replace(/[\\/:*?"<>|\u0000-\u001f\u007f]/g, '_').trim()
  return cleaned !== '' ? cleaned : 'file'
}
