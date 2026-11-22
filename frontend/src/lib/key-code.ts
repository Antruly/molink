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

/// Names (and aliases) that are not a single printable character. `Win`,
/// `Super`, `OS` and `超` all describe the same physical left Windows key.
const NAMED: Record<string, string> = {
  Enter: 'Enter',
  Backspace: 'Backspace',
  Tab: 'Tab',
  Escape: 'Escape',
  Delete: 'Delete',
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
export function keyToCode(key: string): string {
  if (key.length === 1) {
    if (key >= 'a' && key <= 'z') return 'Key' + key.toUpperCase()
    if (key >= 'A' && key <= 'Z') return 'Key' + key
    if (key >= '0' && key <= '9') return 'Digit' + key
    return PUNCTUATION[key] ?? ''
  }
  return NAMED[key] ?? ''
}
