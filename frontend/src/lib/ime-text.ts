/// Committed IME text is sent to the host as `{ k: "text", text }`.
///
/// The host rejects a text event larger than 4096 UTF-8 bytes, so a long
/// composition (or a paste-like IME commit) must be split instead of dropped.
/// Splitting is done on code points, never in the middle of a UTF-8 sequence
/// or a surrogate pair.

export const INPUT_TEXT_MAX_BYTES = 4096

const encoder = new TextEncoder()

export function utf8ByteLength(text: string): number {
  return encoder.encode(text).byteLength
}

/// Split @p text into chunks of at most @p limit UTF-8 bytes each. Empty text
/// yields no chunks. The concatenation of the result is always the input.
export function splitImeText(
  text: string,
  limit: number = INPUT_TEXT_MAX_BYTES,
): string[] {
  if (!text) return []
  if (!Number.isFinite(limit) || limit < 4) {
    throw new RangeError('the text chunk limit must be at least 4 bytes')
  }
  const chunks: string[] = []
  let current = ''
  let bytes = 0
  // `for...of` walks Unicode code points, so a surrogate pair stays together.
  for (const character of text) {
    const size = encoder.encode(character).byteLength
    if (bytes + size > limit) {
      if (current) chunks.push(current)
      current = character
      bytes = size
    } else {
      current += character
      bytes += size
    }
  }
  if (current) chunks.push(current)
  return chunks
}
