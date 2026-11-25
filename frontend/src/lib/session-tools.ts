import type { ClientPhase } from './client'

/// The three panels a session can open beside the picture.
export type SessionPanelKind = 'chat' | 'files' | 'terminal'

export interface SessionToolEntry {
  key: SessionPanelKind
  label: string
}

/// The terminal endpoint authenticates with the host session token, so the
/// entry only appears once the welcome has made the session live. A host that
/// answered `terminal_disabled` normally never shows it again for that
/// session, but the state is deliberately false until the panel itself
/// reports the refusal: an unset or unknown capability must not hide the
/// entry (a host that has no terminal answers when the panel opens).
export function terminalEntryVisible(
  phase: ClientPhase,
  terminalDisabled: boolean,
): boolean {
  return phase === 'live' && !terminalDisabled
}

/// Chat, files and terminal in display order. Both the wide toolbar and the
/// compact tools menu render this list, which is what keeps the terminal
/// entry on both paths instead of only one of them.
export function sessionToolEntries(options: {
  phase: ClientPhase
  terminalDisabled: boolean
  chatLabel: string
  filesLabel: string
  terminalLabel: string
}): SessionToolEntry[] {
  const entries: SessionToolEntry[] = [
    { key: 'chat', label: options.chatLabel },
    { key: 'files', label: options.filesLabel },
  ]
  if (terminalEntryVisible(options.phase, options.terminalDisabled)) {
    entries.push({ key: 'terminal', label: options.terminalLabel })
  }
  return entries
}
