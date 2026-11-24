/// Remote shortcut sequences for keys the local desktop consumes before they
/// can reach the page. On Cinnamon/GNOME the window manager grabs Super
/// (MetaLeft) itself, so the browser never sees a Win key press. The toolbar
/// sends these combinations to the host directly; the protocol is unchanged —
/// each entry is still a sequence of ordinary key down/up events, and the host
/// already maps MetaLeft to VK_LWIN.

export interface KeyStep {
  code: string
  down: boolean
}

export interface KeyCombo {
  id: string
  /// KeyboardEvent.code values, modifiers first and the main key last.
  keys: readonly string[]
  /// Key names shown in the menu, e.g. "Win+Shift+S".
  display: string
  /// i18n key describing what the combination does.
  labelKey: string
  /// Extra confirmation shown before sending, for shortcuts with side effects.
  confirmKey?: string
}

export const KEY_COMBOS: readonly KeyCombo[] = [
  { id: 'win', keys: ['MetaLeft'], display: 'Win', labelKey: 'session.combo.win' },
  { id: 'win-r', keys: ['MetaLeft', 'KeyR'], display: 'Win+R', labelKey: 'session.combo.run' },
  { id: 'win-e', keys: ['MetaLeft', 'KeyE'], display: 'Win+E', labelKey: 'session.combo.explorer' },
  { id: 'win-d', keys: ['MetaLeft', 'KeyD'], display: 'Win+D', labelKey: 'session.combo.desktop' },
  { id: 'win-tab', keys: ['MetaLeft', 'Tab'], display: 'Win+Tab', labelKey: 'session.combo.taskView' },
  {
    id: 'win-l',
    keys: ['MetaLeft', 'KeyL'],
    display: 'Win+L',
    labelKey: 'session.combo.lock',
    confirmKey: 'session.combo.lockConfirm',
  },
  { id: 'alt-tab', keys: ['AltLeft', 'Tab'], display: 'Alt+Tab', labelKey: 'session.combo.switchWindow' },
  { id: 'alt-f4', keys: ['AltLeft', 'F4'], display: 'Alt+F4', labelKey: 'session.combo.closeWindow' },
  { id: 'ctrl-esc', keys: ['ControlLeft', 'Escape'], display: 'Ctrl+Esc', labelKey: 'session.combo.startMenu' },
  {
    id: 'ctrl-shift-esc',
    keys: ['ControlLeft', 'ShiftLeft', 'Escape'],
    display: 'Ctrl+Shift+Esc',
    labelKey: 'session.combo.taskManager',
  },
  {
    id: 'win-shift-s',
    keys: ['MetaLeft', 'ShiftLeft', 'KeyS'],
    display: 'Win+Shift+S',
    labelKey: 'session.combo.screenshot',
  },
  { id: 'print-screen', keys: ['PrintScreen'], display: 'PrintScreen', labelKey: 'session.combo.printScreen' },
]

/// One combination is one press/release block: modifiers go down in the order
/// written, the main key is pressed and released, then the modifiers come back
/// up in reverse order. Every key that goes down is released exactly once, so
/// the remote never keeps a modifier held.
export function keySequence(keys: readonly string[]): KeyStep[] {
  return [
    ...keys.map((code) => ({ code, down: true })),
    ...[...keys].reverse().map((code) => ({ code, down: false })),
  ]
}

export function findKeyCombo(id: string): KeyCombo | undefined {
  return KEY_COMBOS.find((combo) => combo.id === id)
}

export interface KeyForwardState {
  hasFocus: boolean
  visible: boolean
  controlEnabled: boolean
  localTarget: boolean
  settingsOpen: boolean
  /// A toolbar popover (the quality panel) is open. It is portaled outside
  /// the remote surface, so a focused slider would otherwise still send keys
  /// to the host.
  localPanelOpen?: boolean
  /// Linux X11 keyboard-grab mode only: the shell keeps the grab while the
  /// pointer is over the remote picture, and the page must mirror that so keys
  /// do not keep going to the host after the pointer left it. `undefined` on
  /// every other platform, which leaves their behaviour unchanged.
  remotePictureHover?: boolean
}

/// Keys reach the host only while the remote picture is the active input
/// target. A hidden/background window, a local form field, an open toolbar
/// panel, or the settings dialog keeps the keyboard local.
export function shouldForwardKeys(state: KeyForwardState): boolean {
  return (
    state.hasFocus &&
    state.visible &&
    state.controlEnabled &&
    !state.localTarget &&
    !state.settingsOpen &&
    !state.localPanelOpen &&
    state.remotePictureHover !== false
  )
}
