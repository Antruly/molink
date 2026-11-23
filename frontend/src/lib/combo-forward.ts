/// First-use prompt for combo forwarding.
///
/// "Combo forwarding" is the X11 keyboard grab in the Linux desktop shell:
/// without it the window manager eats Win+R / Win+E / Alt+Tab, so the toolbar
/// sends those sequences itself but physical Win shortcuts never reach the
/// remote. The first time the user reaches for a combo, ask once whether to
/// enable it. A remembered answer is persistent; an unremembered answer only
/// applies to the current page session.
export type ComboForwardChoice = 'enabled' | 'declined'

export function normalizeComboForwardAsked(
  raw: unknown,
): ComboForwardChoice | null {
  return raw === 'enabled' || raw === 'declined' ? raw : null
}

export interface ComboForwardPromptState {
  /// The shell can grab the keyboard at all (X11 Linux shell). A plain
  /// browser has nothing to enable, so it never prompts.
  applicable: boolean
  /// Combo forwarding is currently on.
  enabled: boolean
  /// The remembered answer (`null` = never answered with "remember").
  asked: ComboForwardChoice | null
  /// The user already answered this page session without remembering.
  answeredThisSession: boolean
}

/// Ask exactly when forwarding is still off, there is no remembered answer,
/// and this page session has not answered yet.
export function shouldAskComboForward(
  state: ComboForwardPromptState,
): boolean {
  return state.applicable && !state.enabled && state.asked === null &&
    !state.answeredThisSession
}

export interface ComboForwardAnswer {
  enabled: boolean
  /// Written to storage only when "remember" was ticked.
  asked: ComboForwardChoice | null
  answeredThisSession: true
}

/// Applies the dialog's two buttons and the remember checkbox. `remember`
/// false leaves `asked` null: this session stops asking, the next launch asks
/// again.
export function comboForwardAnswer(
  enable: boolean,
  remember: boolean,
): ComboForwardAnswer {
  return {
    enabled: enable,
    asked: remember ? (enable ? 'enabled' : 'declined') : null,
    answeredThisSession: true,
  }
}
