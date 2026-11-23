/// How a "choose folder" button can actually work on this page.
///
/// The browser's File System Access API (`showDirectoryPicker`) exists only in
/// a secure context and only in Chromium. The GTK/WebKit desktop shell does
/// not implement it, and WebKit treats `<input webkitdirectory>` as an
/// ordinary file picker, so the shell must open a native folder dialog and
/// send the absolute path back through its JS bridge. A plain insecure
/// browser with neither option must say so instead of opening a file picker
/// that silently loses the folder shape.
export type DirectoryPickMode =
  | 'fs-access'
  | 'shell'
  | 'directory-input'
  | 'unsupported'

export interface DirectoryPickEnvironment {
  /// `window.isSecureContext`: the File System Access API is absent or throws
  /// on a plain-http page even when the browser implements it.
  secureContext: boolean
  showDirectoryPicker?: unknown
  /// The Linux shell's `window.webkit.messageHandlers.molink` bridge.
  shellBridge: boolean
  /// `'webkitdirectory' in document.createElement('input')`.
  supportsDirectoryInput: boolean
}

export function directoryPickMode(
  env: DirectoryPickEnvironment,
): DirectoryPickMode {
  if (env.secureContext && typeof env.showDirectoryPicker === 'function') {
    return 'fs-access'
  }
  if (env.shellBridge) return 'shell'
  if (env.supportsDirectoryInput) return 'directory-input'
  return 'unsupported'
}

/// The i18n key explaining a degraded/unsupported picker. `null` means the
/// native File System Access picker needs no explanation.
export function directoryPickNoteKey(
  mode: DirectoryPickMode,
): string | null {
  if (mode === 'unsupported') return 'files.dirPickerUnsupported'
  if (mode === 'directory-input') return 'files.dirPickerInputFallback'
  if (mode === 'shell') return 'files.dirPickerShell'
  return null
}
