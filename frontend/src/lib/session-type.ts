/// Host desktop session reported by `/api/state` (`session_type`). Linux
/// capture and input are X11 only, so a Wayland host reaches XWayland
/// applications but not native Wayland ones. The core says so; the UI shows a
/// one-time notice instead of letting the user think the feature is broken.

export type SessionType = 'x11' | 'wayland' | 'unknown'

/// Treats anything the core did not send (an older build) as unknown rather
/// than guessing.
export function normalizeSessionType(value: unknown): SessionType {
  return value === 'x11' || value === 'wayland' ? value : 'unknown'
}

/// Same one-shot dismissal convention as the update notice: the key holds
/// `1` once the user has closed the bar, and the bar never returns.
export const WAYLAND_NOTICE_DISMISS_KEY = 'molink.waylandNoticeDismissed'

/// The subset of localStorage needed here, so tests can pass a fake.
export interface PreferenceStorage {
  getItem(key: string): string | null
  setItem(key: string, value: string): void
}

function defaultStorage(): PreferenceStorage | null {
  try {
    if (typeof window !== 'undefined' && window.localStorage) {
      return window.localStorage
    }
  } catch {
    // Private windows and disabled storage behave like an unremembered choice.
  }
  return null
}

export function waylandNoticeDismissed(
  storage: PreferenceStorage | null = defaultStorage(),
): boolean {
  if (storage === null) return false
  try {
    return storage.getItem(WAYLAND_NOTICE_DISMISS_KEY) === '1'
  } catch {
    return false
  }
}

export function dismissWaylandNotice(
  storage: PreferenceStorage | null = defaultStorage(),
): void {
  if (storage === null) return
  try {
    storage.setItem(WAYLAND_NOTICE_DISMISS_KEY, '1')
  } catch {
    // Blocked storage is fine; the notice just reappears next launch.
  }
}

/// The bar is shown only for a Wayland host, and only until it is dismissed.
export function shouldShowWaylandNotice(
  sessionType: unknown,
  dismissed: boolean,
): boolean {
  return normalizeSessionType(sessionType) === 'wayland' && !dismissed
}
