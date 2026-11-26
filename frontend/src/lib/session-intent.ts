/// What a tab was opened for. Every tab renders one page and one page only:
/// a desktop tab owns the canvas, while files/chat/terminal tabs render their
/// panel full-page and never touch the video path.
export type SessionIntent = 'desktop' | 'files' | 'chat' | 'terminal'

/// The three panel kinds that can also live in a desktop drawer.
export type PaneKind = 'files' | 'chat' | 'terminal'

export interface PageFeatures {
  /// The page draws remote pixels (a canvas plus the cursor overlay).
  canvas: boolean
  /// The page offers picture controls (quality, codec, display mode).
  quality: boolean
  /// The page owns the remote keyboard/pointer.
  input: boolean
  /// The embedded panel this page shows, if any.
  panel: PaneKind | null
}

/// Older payloads (or a tab restored from storage) may carry no intent. That
/// is a desktop session, the behaviour that existed before this field.
export function normalizeSessionIntent(value: unknown): SessionIntent {
  if (value === 'files' || value === 'chat' || value === 'terminal') {
    return value
  }
  return 'desktop'
}

export function isPaneIntent(intent: SessionIntent): intent is PaneKind {
  return intent === 'files' || intent === 'chat' || intent === 'terminal'
}

/// The page a tab renders. Kept as one function so the router, the tab label
/// and the tests cannot disagree.
export function pageFeatures(intent: SessionIntent | undefined): PageFeatures {
  const kind = normalizeSessionIntent(intent)
  if (kind === 'desktop') {
    return { canvas: true, quality: true, input: true, panel: null }
  }
  return { canvas: false, quality: false, input: false, panel: kind }
}

/// i18n key for the purpose suffix on a tab title, or null for desktop (the
/// desktop tab keeps the plain device name).
export function intentSuffixKey(
  intent: SessionIntent | undefined,
): string | null {
  const kind = normalizeSessionIntent(intent)
  // Short nouns, so three tabs of one host read as
  // `YQYX-OFFICE01`, `YQYX-OFFICE01 · 文件`, `YQYX-OFFICE01 · 终端`.
  if (kind === 'files') return 'session.files'
  if (kind === 'chat') return 'session.chat'
  if (kind === 'terminal') return 'terminal.title'
  return null
}

/// `YQYX-OFFICE01 · 文件` for a non-desktop tab; the bare label for desktop.
/// The suffix is passed in already translated so this stays pure.
export function tabTitle(
  label: string,
  intent: SessionIntent | undefined,
  suffix: string,
): string {
  const key = intentSuffixKey(intent)
  if (key === null || suffix.trim() === '') return label
  return `${label} · ${suffix}`
}

/// A pane belongs to a desktop session's runtime. Promote it to its own tab
/// without creating a second connection; the tab shares the owner's runtime.
export function paneConnectionId(runtimeTabId: string | undefined, id: string): string {
  return runtimeTabId ?? id
}

export interface MinimizableTab {
  id: string
  minimized?: boolean
}

/// The tab to show after `fromId` is minimised or closed. Minimised tabs are
/// skipped, the neighbour before it wins, and an empty set falls back home.
/// The session itself is untouched: minimising is a tab-bar state, not a
/// disconnect, so the pane keeps its buffered output.
export function nextVisibleTabId(
  tabs: MinimizableTab[],
  fromId: string,
): string {
  const index = tabs.findIndex((tab) => tab.id === fromId)
  const candidates = tabs.filter(
    (tab) => tab.id !== fromId && !tab.minimized,
  )
  if (candidates.length === 0) return 'home'
  const at = Math.max(0, Math.min(index - 1, candidates.length - 1))
  return candidates[at]!.id
}
