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
