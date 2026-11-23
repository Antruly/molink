export type ShellPlatform = 'linux' | 'windows' | 'macos' | 'web'

export interface ShellInfo {
  enabled: boolean
  platform: ShellPlatform
}

const params = new URLSearchParams(window.location.search)

export const shellInfo: ShellInfo = {
  enabled: params.get('shell') === '1',
  platform: (params.get('platform') ?? 'web') as ShellPlatform,
}

export type WindowAction =
  | 'minimize'
  | 'maximize'
  | 'fullscreen'
  | 'fullscreen-on'
  | 'fullscreen-off'
  | 'close'
  | 'drag'
  | 'quit'
  | 'resize-top'
  | 'resize-bottom'
  | 'resize-left'
  | 'resize-right'
  | 'resize-tl'
  | 'resize-tr'
  | 'resize-bl'
  | 'resize-br'

interface WebKitWindow {
  webkit?: {
    messageHandlers?: {
      molink?: { postMessage(message: unknown): void }
    }
  }
  chrome?: {
    webview?: { postMessage(message: string): void }
  }
  /// Injected by the Linux shell on every page load: false means Wayland (or
  /// a GTK build without X11), where an application cannot grab the keyboard.
  __molinkGrabSupported?: boolean
}

export type KeyboardGrabStatus = 'unknown' | 'on' | 'off' | 'unsupported' | 'failed'

let grabSupport: 'unknown' | 'available' | 'unsupported' = 'unknown'
let lastGrabStatus: KeyboardGrabStatus = 'unknown'
const grabListeners = new Set<() => void>()

function readGrabSupport(): void {
  const target = window as unknown as WebKitWindow
  if (target.__molinkGrabSupported === false) {
    grabSupport = 'unsupported'
  } else if (target.__molinkGrabSupported === true) {
    grabSupport = 'available'
  }
}

if (typeof window !== 'undefined') {
  window.addEventListener('molink-grab', ((event: Event) => {
    const detail = (event as CustomEvent<unknown>).detail
    if (typeof detail !== 'string') return
    lastGrabStatus = detail as KeyboardGrabStatus
    if (detail === 'unsupported') {
      grabSupport = 'unsupported'
    } else if (detail === 'on' || detail === 'off' || detail === 'failed') {
      grabSupport = 'available'
    }
    for (const listener of grabListeners) listener()
  }) as EventListener)
}

function postShellCommand(command: string): void {
  const target = window as unknown as WebKitWindow
  if (target.webkit?.messageHandlers?.molink) {
    target.webkit.messageHandlers.molink.postMessage(command)
    return
  }
  if (target.chrome?.webview) {
    target.chrome.webview.postMessage(command)
  }
}

/// Sends a window action to the native shell. No-op in a plain browser.
export function windowAction(action: WindowAction): void {
  postShellCommand(action)
}

/// The Linux shell exposes an X11 keyboard grab over the WebKit bridge. A
/// plain browser, a popout page without the bridge and the Windows shell are
/// all "not applicable"; Wayland is "not available" (the switch is greyed).
export function keyboardGrabApplicable(): boolean {
  const target = window as unknown as WebKitWindow
  return shellInfo.platform === 'linux' && Boolean(target.webkit?.messageHandlers?.molink)
}

export function keyboardGrabAvailable(): boolean {
  if (!keyboardGrabApplicable()) return false
  readGrabSupport()
  return grabSupport !== 'unsupported'
}

/// Current on/off/unsupported state reported by the shell, for the settings
/// hint. "unknown" until the first status arrives.
export function keyboardGrabStatus(): KeyboardGrabStatus {
  return lastGrabStatus
}

export function subscribeKeyboardGrab(listener: () => void): () => void {
  grabListeners.add(listener)
  return () => {
    grabListeners.delete(listener)
  }
}

/// Asks the shell to take/release the X11 keyboard grab. The shell still owns
/// every safety release; this only expresses the page's wish.
export function setKeyboardGrab(on: boolean): void {
  if (!keyboardGrabApplicable()) return
  if (on && !keyboardGrabAvailable()) return
  postShellCommand(on ? 'grab-keyboard' : 'release-keyboard')
}

/// Asks the native shell to open the local diagnostics directory. The shell
/// resolves the path itself, so a remote host's path is never trusted here.
/// Returns false in a plain browser, where there is nothing to open.
export function openLogDirectory(): boolean {
  if (!shellInfo.enabled) return false
  postShellCommand('open-log-dir')
  return true
}

/// Opens a URL in the system browser (not an in-app window).
export function openExternal(url: string): void {
  if (!url) return
  if (shellInfo.enabled) {
    postShellCommand(`open-url:${url}`)
    return
  }
  window.open(url, '_blank', 'noopener,noreferrer')
}

/// Asks the Windows shell to download and run the setup executable.
export function installUpdate(url: string): void {
  if (!url) return
  postShellCommand(`update-url:${url}`)
}

/// Sets the native window's fullscreen state instead of toggling it, so two
/// independent UI surfaces (session immersive mode and the title bar) cannot
/// drift apart.
export function setWindowFullscreen(on: boolean): void {
  windowAction(on ? 'fullscreen-on' : 'fullscreen-off')
}

export interface ShellDragOutPayload {
  /// Absolute staging folder under the local cache; the shell removes it
  /// after the drag if the user cancels.
  dir: string
  /// Absolute local files, one per remote entry (folders expanded).
  paths: string[]
  /// Display names in the same order; only used for the drag icon/fallback.
  names: string[]
}

/// The Linux GTK shell can open a native folder chooser. WebKitGTK does not
/// implement the File System Access API and treats `webkitdirectory` as a
/// plain file picker, so this bridge is the only correct folder picker there.
export function shellDirectoryPickerCapable(): boolean {
  const target = window as unknown as WebKitWindow
  return shellInfo.enabled && shellInfo.platform === 'linux' &&
    Boolean(target.webkit?.messageHandlers?.molink)
}

/// Asks the shell to open `gtk_file_chooser_dialog_new(... SELECT_FOLDER ...)`.
/// The chosen absolute path arrives later as a `molink-directory` DOM event;
/// cancelling dispatches nothing.
export function requestShellDirectoryPick(): boolean {
  if (!shellDirectoryPickerCapable()) return false
  postShellCommand('pick-directory')
  return true
}

/// Listens for the absolute folder path the native picker chose.
export function onShellDirectoryPicked(
  listener: (path: string) => void,
): () => void {
  const handler = (event: Event) => {
    const detail = (event as CustomEvent<unknown>).detail
    if (typeof detail !== 'string' || detail === '') return
    listener(detail)
  }
  window.addEventListener('molink-directory', handler as EventListener)
  return () => {
    window.removeEventListener('molink-directory', handler as EventListener)
  }
}

/// Only the Linux GTK shell can start a system drag from a WebView: the
/// browser page has no API for it, and WebView2/Windows is a separate shell
/// that does not implement the bridge command yet.
export function shellDragOutCapable(): boolean {
  const target = window as unknown as WebKitWindow
  return shellInfo.enabled && shellInfo.platform === 'linux' &&
    Boolean(target.webkit?.messageHandlers?.molink)
}

/// Hands the prepared local files to the shell, which starts the native
/// `text/uri-list` drag at the pointer. Returns false when no bridge exists so
/// the caller can fall back to a normal download.
export function startShellDragOut(payload: ShellDragOutPayload): boolean {
  if (!shellDragOutCapable()) return false
  if (payload.paths.length === 0) return false
  postShellCommand(`drag-out:${JSON.stringify(payload)}`)
  return true
}
