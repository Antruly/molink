/// Pure rules for downloading a file the host copied (clipboard-pull).
///
/// The host announces `{"type":"clipboard-sync","files":[{id,name,size}]}`.
/// The page stages each file through the local core and asks for 256 KiB
/// ranges, which come back as 0x18 packets. This module holds the size limits,
/// the batch planning and the per-chunk checks so they can be unit tested
/// without a browser.

/// Matches the host's per-selection guidance and the files panel.
export const PULL_MAX_FILES = 8
export const PULL_MAX_TOTAL_BYTES = 2 * 1024 * 1024 * 1024
export const PULL_MAX_FILE_BYTES = 256 * 1024 * 1024
export const PULL_CHUNK_BYTES = 256 * 1024
/// Two files at a time: enough to hide RTT latency without flooding the
/// reliable channel that also carries the screen.
export const PULL_CONCURRENCY = 2

export interface PullFile {
  id: number
  name: string
  size: number
  path?: string
}

export interface PullPlan {
  files: PullFile[]
  /// Files dropped because of the count/byte limits. The caller tells the
  /// user; the first prefix still syncs.
  omitted: number
}

function validFile(value: unknown): PullFile | null {
  if (typeof value !== 'object' || value === null) return null
  const record = value as {
    id?: unknown
    name?: unknown
    size?: unknown
    path?: unknown
  }
  if (
    typeof record.id !== 'number' || !Number.isSafeInteger(record.id) ||
    record.id <= 0
  ) {
    return null
  }
  if (typeof record.name !== 'string' || record.name === '') return null
  if (
    typeof record.size !== 'number' || !Number.isSafeInteger(record.size) ||
    record.size < 0
  ) {
    return null
  }
  const file: PullFile = {
    id: record.id,
    name: record.name,
    size: record.size,
  }
  if (typeof record.path === 'string' && record.path !== '') {
    file.path = record.path
  }
  return file
}

/// Keep the first files that fit the count and byte budgets. Unlike the old
/// "all or nothing" cap, a selection with one huge file still copies the
/// small files before it; the caller reports the truncation.
export function planPullFiles(raw: unknown): PullPlan {
  if (!Array.isArray(raw)) return { files: [], omitted: 0 }
  const parsed: PullFile[] = []
  for (const item of raw) {
    const file = validFile(item)
    if (file !== null) parsed.push(file)
  }
  const files: PullFile[] = []
  let total = 0
  for (const file of parsed) {
    if (files.length >= PULL_MAX_FILES) break
    if (file.size > PULL_MAX_FILE_BYTES) break
    if (total + file.size > PULL_MAX_TOTAL_BYTES) break
    files.push(file)
    total += file.size
  }
  return { files, omitted: parsed.length - files.length }
}

/// Bytes to ask for at @p offset. Zero means the file is complete.
export function pullChunkLength(size: number, offset: number): number {
  if (!Number.isSafeInteger(size) || !Number.isSafeInteger(offset)) return 0
  if (offset < 0 || offset >= size) return 0
  return Math.min(PULL_CHUNK_BYTES, size - offset)
}

export type PullChunkCheck =
  | { ok: true }
  | { ok: false; reason: 'gone' | 'failed' | 'incomplete' }

/// Validate one 0x18 reply before writing its bytes. A short read is treated
/// as a failure, never as end-of-file: the staging entry declared the size.
export function checkPullChunk(
  status: number,
  chunkOffset: number,
  expectedOffset: number,
  expectedLength: number,
  byteLength: number,
): PullChunkCheck {
  if (status === 2) return { ok: false, reason: 'gone' }
  if (status !== 0) return { ok: false, reason: 'failed' }
  if (chunkOffset !== expectedOffset || byteLength !== expectedLength) {
    return { ok: false, reason: 'incomplete' }
  }
  return { ok: true }
}

/// Batch directory name for `~/.cache/molink/clipboard/<sha1>/`. The input
/// mixes a per-copy nonce with the file list, so copying the same files twice
/// gets two directories (and no `name (1).txt` collisions).
export function batchKeyMaterial(
  files: readonly PullFile[],
  nonce: string,
): string {
  return `${nonce}|${files.map((file) => `${file.name}:${file.size}`).join('|')}`
}

/// Node and non-secure debug origins have no `crypto.subtle`. This is only a
/// cache directory name, not a security boundary; FNV-1a keeps the shape
/// (lowercase hex, 16 chars) when SHA-1 is unavailable.
export function fallbackBatchId(material: string): string {
  let first = 2166136261
  let second = 84696351
  for (let i = 0; i < material.length; i += 1) {
    const code = material.charCodeAt(i)
    first ^= code
    first = Math.imul(first, 16777619)
    second ^= code + 0x9e37
    second = Math.imul(second, 2246822519)
  }
  const hex = (value: number) => (value >>> 0).toString(16).padStart(8, '0')
  return hex(first) + hex(second)
}

export async function clipboardBatchId(
  files: readonly PullFile[],
  nonce: string,
  subtle: SubtleCrypto | null = typeof crypto !== 'undefined'
    ? (crypto.subtle ?? null)
    : null,
): Promise<string> {
  const material = batchKeyMaterial(files, nonce)
  if (subtle !== null && typeof subtle.digest === 'function') {
    try {
      const digest = await subtle.digest(
        'SHA-1',
        new TextEncoder().encode(material),
      )
      return Array.from(new Uint8Array(digest))
        .map((byte) => byte.toString(16).padStart(2, '0'))
        .join('')
    } catch {
      // Fall through to the deterministic fallback.
    }
  }
  return fallbackBatchId(material)
}
