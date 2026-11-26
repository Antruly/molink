/// The ways the simple home can start a session. This lives in `lib/` with no
/// React imports so `node --test` can pin the list the connect card and the
/// recent-device shortcut buttons both render.

export type SessionIntent = 'desktop' | 'files' | 'chat' | 'terminal'

export interface SessionMode {
  intent: SessionIntent
  /// Locale key under `simple.*`, shared by every place that offers the mode.
  labelKey: string
}

export const SESSION_MODES: readonly SessionMode[] = [
  { intent: 'desktop', labelKey: 'simple.modeDesktop' },
  { intent: 'files', labelKey: 'simple.modeFiles' },
  { intent: 'chat', labelKey: 'simple.modeChat' },
  { intent: 'terminal', labelKey: 'simple.modeTerminal' },
]
