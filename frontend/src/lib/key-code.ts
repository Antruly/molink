/// Pure translation from a browser keyboard event to the physical
/// `KeyboardEvent.code` names the host understands.
///
/// `code` is reliable on physical keyboards but mobile virtual keyboards often
/// report `Unidentified`, and older WebKit builds use the pre-standard
/// `OSLeft`/`OSRight` spelling for the Windows key. The host accepts the
/// legacy aliases too; canonicalising here keeps both directions honest.

const PUNCTUATION: Record<string, string> = {
  ' ': 'Space',
  '\n': 'Enter',
  '-': 'Minus',
  '=': 'Equal',
  '[': 'BracketLeft',
  ']': 'BracketRight',
  '\\': 'Backslash',
  ';': 'Semicolon',
  "'": 'Quote',
  '`': 'Backquote',
  ',': 'Comma',
  '.': 'Period',
  '/': 'Slash',
}

/// The keypad reports the same printable `event.key` as the main row, so the
/// fallback needs `KeyboardEvent.location` (3 = DOM_KEY_LOCATION_NUMPAD) to
/// avoid turning Numpad1 into Digit1. Numbers, operators and Enter all have a
/// keypad-specific physical code.
const NUMPAD: Record<string, string> = {
  '0': 'Numpad0',
  '1': 'Numpad1',
  '2': 'Numpad2',
  '3': 'Numpad3',
  '4': 'Numpad4',
  '5': 'Numpad5',
  '6': 'Numpad6',
  '7': 'Numpad7',
  '8': 'Numpad8',
  '9': 'Numpad9',
  '+': 'NumpadAdd',
  '-': 'NumpadSubtract',
  '*': 'NumpadMultiply',
  '/': 'NumpadDivide',
  '.': 'NumpadDecimal',
  ',': 'NumpadComma',
  '=': 'NumpadEqual',
  Enter: 'NumpadEnter',
  Separator: 'NumpadSeparator',
  Decimal: 'NumpadDecimal',
}

/// Names (and aliases) that are not a single printable character. `Win`,
/// `Super`, `OS` and `超` all describe the same physical left Windows key.
/// Identity entries keep media/browser/IME keys alive when `event.code` is
/// missing and the browser only reports the friendly name.
const NAMED: Record<string, string> = {
  Enter: 'Enter',
  Backspace: 'Backspace',
  Tab: 'Tab',
  Escape: 'Escape',
  Delete: 'Delete',
  Insert: 'Insert',
  Home: 'Home',
  End: 'End',
  PageUp: 'PageUp',
  PageDown: 'PageDown',
  ArrowLeft: 'ArrowLeft',
  ArrowRight: 'ArrowRight',
  ArrowUp: 'ArrowUp',
  ArrowDown: 'ArrowDown',
  Meta: 'MetaLeft',
  OS: 'MetaLeft',
  Win: 'MetaLeft',
  Super: 'MetaLeft',
  '超': 'MetaLeft',
  OSLeft: 'MetaLeft',
  OSRight: 'MetaRight',
  AltGraph: 'AltGraph',
  CapsLock: 'CapsLock',
  NumLock: 'NumLock',
  ScrollLock: 'ScrollLock',
  PrintScreen: 'PrintScreen',
  Pause: 'Pause',
  ContextMenu: 'ContextMenu',
  Help: 'Help',
  Select: 'Select',
  Execute: 'Execute',
  Clear: 'Clear',
  Again: 'Again',
  Undo: 'Undo',
  Cut: 'Cut',
  Copy: 'Copy',
  Paste: 'Paste',
  Find: 'Find',
  Cancel: 'Cancel',
  Power: 'Power',
  Sleep: 'Sleep',
  WakeUp: 'WakeUp',
  AudioVolumeMute: 'AudioVolumeMute',
  AudioVolumeDown: 'AudioVolumeDown',
  AudioVolumeUp: 'AudioVolumeUp',
  MediaTrackNext: 'MediaTrackNext',
  MediaTrackPrevious: 'MediaTrackPrevious',
  MediaStop: 'MediaStop',
  MediaPlayPause: 'MediaPlayPause',
  BrowserBack: 'BrowserBack',
  BrowserForward: 'BrowserForward',
  BrowserRefresh: 'BrowserRefresh',
  BrowserStop: 'BrowserStop',
  BrowserSearch: 'BrowserSearch',
  BrowserFavorites: 'BrowserFavorites',
  BrowserHome: 'BrowserHome',
  LaunchMail: 'LaunchMail',
  LaunchApp1: 'LaunchApp1',
  LaunchApp2: 'LaunchApp2',
  Convert: 'Convert',
  NonConvert: 'NonConvert',
  KanaMode: 'KanaMode',
  HangulMode: 'HangulMode',
  HanjaMode: 'HanjaMode',
  Lang1: 'Lang1',
  Lang2: 'Lang2',
}

/// Canonicalise a value that is already a `KeyboardEvent.code` (or a legacy
/// alias for one). Unknown values are returned unchanged so a future code can
/// still reach the host instead of being silently dropped here.
export function normalizeKeyCode(code: string): string {
  if (code === '' || code === 'Unidentified') return ''
  const named = NAMED[code]
  if (named !== undefined) return named
  if (code.length === 1) {
    if (code >= 'a' && code <= 'z') return 'Key' + code.toUpperCase()
    if (code >= 'A' && code <= 'Z') return 'Key' + code
    if (code >= '0' && code <= '9') return 'Digit' + code
  }
  return code
}

/// Map a `KeyboardEvent.key` value (the fallback when `event.code` is empty)
/// onto the physical code the host expects. Unknown input returns `''`
/// instead of guessing.
///
/// `location` is `KeyboardEvent.location`; pass `3` for the numeric keypad so
/// its digits and operators keep their Numpad codes. A keypad `Clear`
/// (NumLock off) deliberately stays `Clear`: mapping it to Numpad5 would make
/// the host's NumLock compensation type "5" instead.
export function keyToCode(key: string, location = 0): string {
  if (location === 3) {
    const numpad = NUMPAD[key]
    if (numpad !== undefined) return numpad
  }
  if (key.length === 1) {
    if (key >= 'a' && key <= 'z') return 'Key' + key.toUpperCase()
    if (key >= 'A' && key <= 'Z') return 'Key' + key
    if (key >= '0' && key <= '9') return 'Digit' + key
    return PUNCTUATION[key] ?? ''
  }
  // F1..F24 are their own code names.
  if (/^F([1-9]|1[0-9]|2[0-4])$/.test(key)) return key
  return NAMED[key] ?? ''
}
