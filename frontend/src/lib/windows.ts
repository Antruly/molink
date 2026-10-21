import { shellInfo } from './shell'

export interface SessionWindowTarget {
  host: string
  port: number
  token: string
  label?: string
}

export function sessionWindowUrl(target: SessionWindowTarget): string {
  const params = new URLSearchParams({
    popout: '1',
    host: target.host,
    port: String(target.port),
    token: target.token,
  })
  if (target.label) params.set('label', target.label)
  if (shellInfo.enabled) {
    params.set('shell', '1')
    params.set('platform', shellInfo.platform)
  }
  return `${window.location.pathname}?${params.toString()}`
}

/// Opens a session in its own window. In a native shell the window is created
/// by the shell; a null handle from WebKit is not a failure there.
export function openSessionWindow(target: SessionWindowTarget): boolean {
  const handle = window.open(
    sessionWindowUrl(target),
    '_blank',
    'popup=yes,width=1280,height=820',
  )
  return shellInfo.enabled || handle !== null
}

/// The first connection of a run is opened as its own window; later
/// connections become tabs.
export function claimFirstWindow(): boolean {
  try {
    if (window.sessionStorage.getItem('molink.firstWindow') === '1') {
      return false
    }
    window.sessionStorage.setItem('molink.firstWindow', '1')
    return true
  } catch {
    return false
  }
}
