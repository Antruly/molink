/// Where the page can reach the *local* core's clipboard staging APIs.
///
/// `localMode` is the server's answer to "did this request arrive over
/// loopback?" (`/api/state`). A remote browser opening
/// `http://192.168.2.20:28418` is therefore never allowed to write this
/// machine's clipboard, no matter what origin it claims.
export type ClipboardFilesMode =
  /// Same machine: stage the files and apply them to the local clipboard.
  | 'auto'
  /// Same machine but the user has not opted into automatic staging; the
  /// files panel offers manual downloads as before.
  | 'manual'
  /// A remote page: tell the user to open the session in the local app.
  | 'local-app'

const LOCAL_DEV_ORIGINS = new Set([
  'http://localhost:1420',
  'http://127.0.0.1:1420',
])

export function normalizeOrigin(origin: string): string {
  try {
    return new URL(origin).origin
  } catch {
    return origin.replace(/\/+$/, '')
  }
}

/// The core has to be the machine's own (loopback peer) *and* the page has to
/// be served by it. The Vite dev server is the one explicit exception: the
/// core's CORS allow-list already admits those two origins.
export function isLocalCorePage(
  localMode: boolean,
  pageOrigin: string,
  coreOrigin: string,
): boolean {
  if (!localMode) return false
  const page = normalizeOrigin(pageOrigin)
  return page === normalizeOrigin(coreOrigin) ||
    LOCAL_DEV_ORIGINS.has(page)
}

export function clipboardFilesMode(
  localMode: boolean,
  pageOrigin: string,
  coreOrigin: string,
  autoEnabled: boolean,
): ClipboardFilesMode {
  if (!isLocalCorePage(localMode, pageOrigin, coreOrigin)) return 'local-app'
  return autoEnabled ? 'auto' : 'manual'
}

/// True when a clipboard file is worth staging automatically. The size the
/// host reported must be a safe, non-zero integer; zero-byte entries and
/// directories (reported as size 0) are left to the manual panel.
export function stageableClipboardFile(file: {
  size?: unknown
}): boolean {
  return typeof file.size === 'number' && Number.isSafeInteger(file.size) &&
    file.size > 0
}
