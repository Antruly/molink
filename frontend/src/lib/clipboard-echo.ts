/// Echo suppression for the two clipboard file directions.
///
/// A file copied here is put on the remote clipboard; the remote then reports
/// its own clipboard (with a path this machine has never seen) and, when the
/// reverse switch is on, that would download the same bytes back, stage them
/// locally, and offer them to the remote again -- the `file (1)(2)` loop.
/// Remember what was written (host paths, or the descriptors of a delayed
/// offer) for a short window and skip it on the way back.
export interface EchoStorage {
  getItem(key: string): string | null
  setItem(key: string, value: string): void
  removeItem?(key: string): void
}

export interface EchoEntry {
  key: string
  at: number
}

export const ECHO_TTL_MS = 2 * 60 * 1000
export const ECHO_MAX_ENTRIES = 64
export const ECHO_STORAGE_KEY = 'molink.clipboard.echo.v1'

/// Windows paths are case-insensitive and the same file can be spelled with
/// either separator. Normalising both sides is what makes the record survive
/// the trip through CF_HDROP and JSON.
export function echoPathKey(path: string): string {
  let text = path.trim().replace(/\\/g, '/')
  text = text.replace(/\/{2,}/g, '/')
  if (text.length > 1) text = text.replace(/\/+$/, '')
  return text.toLowerCase()
}

/// Key for a lazy offer: the host may rename on collision (`a (1).txt`), but
/// the name and size are still enough to recognise our own selection when it
/// comes back as descriptors.
export function echoOfferKey(name: string, size: number): string {
  return `offer:${echoPathKey(name)}:${size}`
}

function defaultStorage(): EchoStorage | null {
  try {
    if (typeof localStorage !== 'undefined') {
      return {
        getItem: (key) => localStorage.getItem(key),
        setItem: (key, value) => localStorage.setItem(key, value),
        removeItem: (key) => localStorage.removeItem(key),
      }
    }
  } catch {
    // Private mode or a sandboxed frame: run without persistence.
  }
  return null
}

export class ClipboardEcho {
  private readonly storage: EchoStorage | null
  private readonly now: () => number
  /// Used when localStorage is unavailable (or a test passes no storage).
  private memory: EchoEntry[] = []

  constructor(
    storage: EchoStorage | null = defaultStorage(),
    now: () => number = Date.now,
  ) {
    this.storage = storage
    this.now = now
  }

  /// Records host paths this machine just put on the remote clipboard.
  rememberPaths(paths: readonly string[]): void {
    const keys: string[] = []
    for (const path of paths) {
      if (typeof path !== 'string' || path.trim() === '') continue
      keys.push(echoPathKey(path))
    }
    this.rememberKeys(keys)
  }

  /// Records the descriptors of a delayed offer. The host may materialise the
  /// files under different paths, so matching falls back to name+size.
  rememberFiles(files: readonly { name?: unknown; size?: unknown }[]): void {
    const keys: string[] = []
    for (const file of files) {
      if (typeof file.name !== 'string' || file.name === '') continue
      if (typeof file.size !== 'number' || !Number.isFinite(file.size)) continue
      keys.push(echoOfferKey(file.name, file.size))
    }
    this.rememberKeys(keys)
  }

  suppressPath(path: string): boolean {
    if (typeof path !== 'string' || path.trim() === '') return false
    return this.has(echoPathKey(path))
  }

  /// True only when every entry is a remembered lazy offer. A batch with one
  /// genuinely new file must still be staged.
  suppressFiles(files: readonly { name?: unknown; size?: unknown }[]): boolean {
    if (files.length === 0) return false
    let matched = 0
    for (const file of files) {
      if (typeof file.name !== 'string' || file.name === '') return false
      if (typeof file.size !== 'number' || !Number.isFinite(file.size)) {
        return false
      }
      if (!this.has(echoOfferKey(file.name, file.size))) return false
      matched += 1
    }
    return matched > 0
  }

  clear(): void {
    this.memory = []
    if (this.storage?.removeItem) this.storage.removeItem(ECHO_STORAGE_KEY)
  }

  private rememberKeys(keys: string[]): void {
    if (keys.length === 0) return
    const at = this.now()
    const entries = this.prune(this.read(), at)
    for (const key of keys) {
      const existing = entries.findIndex((entry) => entry.key === key)
      if (existing >= 0) entries.splice(existing, 1)
      entries.push({ key, at })
    }
    // Newest last, oldest first: dropping from the front keeps the most
    // recently touched 64 entries.
    while (entries.length > ECHO_MAX_ENTRIES) entries.shift()
    this.write(entries)
  }

  private has(key: string): boolean {
    const at = this.now()
    const entries = this.prune(this.read(), at)
    const found = entries.some((entry) => entry.key === key)
    // Persist the pruned list so an expired record cannot accumulate forever.
    this.write(entries)
    return found
  }

  private prune(entries: EchoEntry[], at: number): EchoEntry[] {
    return entries.filter(
      (entry) => typeof entry.at === 'number' && at >= entry.at &&
        at - entry.at < ECHO_TTL_MS,
    )
  }

  private read(): EchoEntry[] {
    if (this.storage === null) return this.memory.slice()
    try {
      const raw = this.storage.getItem(ECHO_STORAGE_KEY)
      if (!raw) return []
      const parsed: unknown = JSON.parse(raw)
      if (!Array.isArray(parsed)) return []
      const entries: EchoEntry[] = []
      for (const item of parsed) {
        if (typeof item !== 'object' || item === null) continue
        const record = item as { key?: unknown; at?: unknown }
        if (typeof record.key !== 'string') continue
        if (typeof record.at !== 'number' || !Number.isFinite(record.at)) {
          continue
        }
        entries.push({ key: record.key, at: record.at })
      }
      return entries
    } catch {
      return []
    }
  }

  private write(entries: EchoEntry[]): void {
    if (this.storage === null) {
      this.memory = entries.slice()
      return
    }
    try {
      if (entries.length === 0) {
        this.storage.removeItem?.(ECHO_STORAGE_KEY)
        return
      }
      this.storage.setItem(ECHO_STORAGE_KEY, JSON.stringify(entries))
    } catch {
      // Quota or a storage that became unavailable: keep working in memory.
      this.memory = entries.slice()
    }
  }
}
