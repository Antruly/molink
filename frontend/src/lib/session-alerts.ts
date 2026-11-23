/// Routing rules for failures reported to a session tab.
///
/// A tab has two independent feedback channels:
///   - `toast` — a transient corner message for a failure that belongs to one
///     feature (the clipboard, a single input event, one file action). The
///     session is still live, so it must never look like a disconnect and must
///     never offer a reconnect.
///   - `error` — the session banner. It is reserved for failures that make the
///     session itself unusable; only some of those can be fixed by reconnecting.
///
/// Keeping the classification here (instead of inside the view) means the
/// connection layer and the tests agree on what "the link is broken" means.

export type ToastTone = 'error' | 'warn' | 'info'

export interface SessionToast {
  /// Monotonic per tab. The clear timer checks it so an old timer cannot
  /// dismiss a newer message.
  id: number
  message: string
  tone: ToastTone
  /// Epoch ms of the last time this message was (re)raised.
  at: number
}

/// How long a toast stays on screen after its last refresh.
export const TOAST_TTL_MS = 6000

export type ErrorSurface = 'toast' | 'banner'

export interface ErrorRoute {
  surface: ErrorSurface
  tone: ToastTone
  /// Only meaningful for the banner: whether "reconnect" is a sensible action.
  reconnect: boolean
}

/// Codes whose failure is informational rather than an error.
const INFO_TOAST_CODES = new Set([
  'clipboard_files_staged',
  'clipboard_files_uploaded',
  'clipboard_files_compat',
])

/// Codes worth a warning tone: the feature was cut short or refused, but the
/// session and its data are intact.
const WARN_TOAST_CODES = new Set([
  'clipboard_too_large',
  'clipboard_files_too_large',
  'clipboard_files_busy',
  'remote_clipboard_too_large',
  'remote_clipboard_truncated',
])

/// Codes that mean the session cannot continue. These keep the redirect to
/// the code entry form (unauthorized/invalid_token) and may offer reconnect.
const SESSION_ERROR_CODES = new Set([
  'unauthorized',
  'invalid_token',
  'too_many_attempts',
  'ip_banned',
  'locked',
  'access_denied',
  'no_token',
  'hello_failed',
  'hello_timeout',
  'hello_rejected',
  'handshake_failed',
  'handshake_timeout',
  'connection_lost',
  'transport_closed',
  'closed',
])

/// A failure that belongs to one feature instead of the link. Input, clipboard
/// and file errors name their channel, so they are safe to detect by prefix.
export function isLocalFailureCode(code: string): boolean {
  if (code === '') return false
  if (
    code === 'bad_input' ||
    code === 'unknown_input' ||
    code === 'unknown_key' ||
    code === 'bad_key' ||
    code === 'bad_clipboard' ||
    code === 'bad_file_action' ||
    code === 'bad_action' ||
    code === 'unknown_action' ||
    code === 'file_error'
  ) {
    return true
  }
  return (
    code.startsWith('clipboard_') ||
    code.startsWith('remote_clipboard_') ||
    code.startsWith('input_') ||
    code.startsWith('file_')
  )
}

/// True when a session-level failure has an action the user can take by
/// reconnecting: the credential/handshake path, a closed transport, or the
/// transport never coming up. Setting changes (presets, display mode) and
/// decode failures deliberately return false: reconnecting cannot fix them,
/// so the banner shows the text and a dismiss button instead.
export function needsReconnect(
  code?: string | null,
  message?: string | null,
): boolean {
  const key = (code ?? '').trim()
  if (SESSION_ERROR_CODES.has(key)) return true
  const text = message ?? ''
  return /hello|handshake|connection lost|connection closed|connection failed|transport closed|heartbeat|timed out|timeout|closed \(100[08]\)|relay is unavailable|could not reach the device/i
    .test(text)
}

/// The user-facing string for a toast. Protocol codes are kept so the view can
/// translate them (`error.<code>`); host prose is shown as sent.
export function toastMessage(code: string | null | undefined, message: string): string {
  const key = (code ?? '').trim()
  if (key !== '' && (message === '' || message === key)) return key
  return message
}

export function classifyError(
  code?: string | null,
  message?: string | null,
): ErrorRoute {
  const key = (code ?? '').trim()
  if (isLocalFailureCode(key)) {
    const tone: ToastTone = INFO_TOAST_CODES.has(key)
      ? 'info'
      : WARN_TOAST_CODES.has(key)
        ? 'warn'
        : 'error'
    return { surface: 'toast', tone, reconnect: false }
  }
  return {
    surface: 'banner',
    tone: 'error',
    reconnect: needsReconnect(key, message),
  }
}

/// One toast slot per tab: a repeat of the same text refreshes the existing
/// toast (same id, new timestamp) instead of stacking another one.
export function toastFor(
  previous: SessionToast | null | undefined,
  message: string,
  tone: ToastTone,
  id: number,
  now: number,
): SessionToast {
  if (previous && previous.message === message) {
    return { ...previous, tone, at: now }
  }
  return { id, message, tone, at: now }
}

export function toastExpired(
  toast: SessionToast,
  now: number,
  ttl: number = TOAST_TTL_MS,
): boolean {
  return now - toast.at >= ttl
}

/// Whether the timer armed for `id` may clear the toast now. A newer toast
/// (different id) wins, and an early wake-up leaves the toast alone.
export function shouldClearToast(
  current: SessionToast | null | undefined,
  id: number,
  now: number,
): boolean {
  return current !== null && current !== undefined &&
    current.id === id && toastExpired(current, now)
}

export interface FailureState {
  error: string | null
  errorCode: string | null
  toast: SessionToast | null
}

export interface FailureResult extends FailureState {
  surface: ErrorSurface
  reconnect: boolean
}

/// Pure state transition used by the store's failure handler. Local failures
/// leave `error`/`errorCode` exactly as they were and only raise the toast;
/// session-level failures replace the banner and keep any toast showing.
export function applyFailure(
  state: FailureState,
  message: string,
  code: string | null,
  id: number,
  now: number,
  tone?: ToastTone,
): FailureResult {
  const route = classifyError(code, message)
  if (route.surface === 'toast') {
    return {
      error: state.error,
      errorCode: state.errorCode,
      toast: toastFor(
        state.toast,
        toastMessage(code, message),
        tone ?? route.tone,
        id,
        now,
      ),
      surface: 'toast',
      reconnect: false,
    }
  }
  return {
    error: message,
    errorCode: code,
    toast: state.toast,
    surface: 'banner',
    reconnect: route.reconnect,
  }
}
