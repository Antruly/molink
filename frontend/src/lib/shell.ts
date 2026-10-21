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

/// Sends a window action to the native shell. No-op in a plain browser.
export function windowAction(action: WindowAction): void {
  const target = window as unknown as WebKitWindow
  if (target.webkit?.messageHandlers?.molink) {
    target.webkit.messageHandlers.molink.postMessage(action)
    return
  }
  if (target.chrome?.webview) {
    target.chrome.webview.postMessage(action)
  }
}
