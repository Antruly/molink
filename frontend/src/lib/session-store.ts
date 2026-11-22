/// Reconnect credentials ("session tokens") handed out by the host in
/// `welcome`. The host keeps a token usable for seven days and refreshes it
/// when it is presented, so remembering it is what lets a viewer restart the
/// client (or the whole machine) without spending another single-use code.
///
/// Storage is deliberately per-target: `molink.session.<host>:<port>` holds
/// `{token, savedAt}`. A token therefore cannot leak into a connection to a
/// different machine or port.
///
/// Two persistent backends exist and only one is used at a time:
///   - the native shell/local UI persists into the core's `ui.json` (via
///     `/api/ui-preferences`), which survives both the shell and the core;
///   - a plain browser persists into localStorage.
/// The in-memory map is always the source of truth for reads, so callers keep
/// the synchronous API the connection code needs.

export interface StoredSession {
  token: string
  savedAt: number
}

export type SessionRecords = Record<string, StoredSession>

/// Keep in step with the host's `kSessionLifetimeMs` (auth_state.cpp).
export const kSessionLifetimeMs = 7 * 24 * 60 * 60 * 1000

export const kSessionKeyPrefix = 'molink.session.'
export const kRememberDeviceKey = 'molink.rememberDevice'

/// The subset of localStorage this module needs, so tests can pass a fake.
export interface SessionStorage {
  readonly length: number
  key(index: number): string | null
  getItem(key: string): string | null
  setItem(key: string, value: string): void
  removeItem(key: string): void
}

type Backend = 'memory' | 'local' | 'external'

interface ExternalBackend {
  /// Persist the complete session map (never partially).
  save(records: SessionRecords): void
}

export function sessionStorageKey(host: string, port: number): string {
  return `${kSessionKeyPrefix}${host}:${port}`
}

function isRecord(value: unknown): value is StoredSession {
  if (typeof value !== 'object' || value === null) return false
  const record = value as { token?: unknown; savedAt?: unknown }
  return (
    typeof record.token === 'string' &&
    record.token !== '' &&
    typeof record.savedAt === 'number' &&
    Number.isFinite(record.savedAt)
  )
}

function isExpired(record: StoredSession, now: number): boolean {
  // A clock that stepped backwards must not invalidate a fresh credential.
  return now - record.savedAt > kSessionLifetimeMs
}

/// Normalizes a persisted map. Accepts both the full key
/// (`molink.session.host:port`) used by this module and a bare `host:port`
/// key, so a hand-edited `ui.json` still works.
function parseRecords(value: unknown, now: number): Map<string, StoredSession> {
  const out = new Map<string, StoredSession>()
  if (typeof value !== 'object' || value === null) return out
  for (const [rawKey, rawRecord] of Object.entries(
    value as Record<string, unknown>,
  )) {
    if (!isRecord(rawRecord)) continue
    if (isExpired(rawRecord, now)) continue
    const key = rawKey.startsWith(kSessionKeyPrefix)
      ? rawKey
      : `${kSessionKeyPrefix}${rawKey}`
    out.set(key, { token: rawRecord.token, savedAt: rawRecord.savedAt })
  }
  return out
}

function defaultStorage(): SessionStorage | null {
  try {
    if (typeof window !== 'undefined' && window.localStorage) {
      return window.localStorage
    }
  } catch {
    // Private windows and disabled storage behave like a memory-only client.
  }
  return null
}

export class SessionStore {
  private readonly memory = new Map<string, StoredSession>()
  private readonly storage: SessionStorage | null
  private readonly clock: () => number
  private backend: Backend = 'memory'
  private external: ExternalBackend | null = null
  private remember = true

  constructor(options: {
    storage?: SessionStorage | null
    now?: () => number
  } = {}) {
    this.storage = options.storage === undefined ? defaultStorage() : options.storage
    this.clock = options.now ?? (() => Date.now())
    this.remember = this.readRememberFlag()
    if (this.storage !== null) {
      this.backend = 'local'
      this.loadLocal()
    }
  }

  private readRememberFlag(): boolean {
    if (this.storage === null) return true
    try {
      return this.storage.getItem(kRememberDeviceKey) !== '0'
    } catch {
      return true
    }
  }

  /// True when a `remember_device: false` preference keeps credentials out of
  /// persistent storage. Memory still receives new tokens so reconnecting a
  /// live tab works.
  get rememberEnabled(): boolean {
    return this.remember
  }

  get backendKind(): Backend {
    return this.backend
  }

  /// Reads the credential for a target, dropping it once its seven days are
  /// up. Expiry is enforced on read so a credential stored before a clock
  /// change (or restored from a backup) cannot be trusted forever.
  tokenFor(host: string, port: number): string {
    if (host === '') return ''
    this.prune()
    return this.memory.get(sessionStorageKey(host, port))?.token ?? ''
  }

  /// Stores a token received in `welcome`. `savedAt` is the receive time, not
  /// the host's issuance time: a reconnect refreshes both.
  rememberToken(
    host: string,
    port: number,
    token: string,
    savedAt = this.clock(),
  ): void {
    if (host === '' || token === '' || !Number.isFinite(savedAt)) return
    this.memory.set(sessionStorageKey(host, port), { token, savedAt })
    this.persist()
  }

  /// Forgets a credential the host rejected.
  forgetToken(host: string, port: number): void {
    if (host === '') return
    const key = sessionStorageKey(host, port)
    const changed = this.memory.delete(key)
    if (!changed && this.backend === 'local') return
    this.persist()
  }

  /// Everything that should be written now. Empty while remembering is off,
  /// so an unrelated preference save cannot resurrect a cleared credential.
  persistedRecords(): SessionRecords {
    if (!this.remember) return {}
    return this.snapshot()
  }

  /// Everything currently held in memory, usable by tests and diagnostics.
  snapshot(): SessionRecords {
    this.prune()
    const out: SessionRecords = {}
    for (const [key, record] of this.memory) out[key] = { ...record }
    return out
  }

  /// Replaces/merges the memory cache from a persisted document (the native
  /// shell's `ui.json`). Expired or malformed entries are discarded. A token
  /// remembered while the document was in flight is newer than the document,
  /// so it wins instead of being overwritten by an older copy.
  hydrate(records: unknown): void {
    for (const [key, record] of parseRecords(records, this.clock())) {
      const existing = this.memory.get(key)
      if (existing !== undefined && existing.savedAt > record.savedAt) continue
      this.memory.set(key, record)
    }
  }

  /// Applies the preferences read from the core. When remembering is off the
  /// stored credentials are not loaded at all, even if an older `ui.json`
  /// still contains them.
  applyStoredPreferences(remember: boolean, sessions: unknown): void {
    this.remember = remember
    if (!remember) {
      this.memory.clear()
      return
    }
    this.hydrate(sessions)
  }

  /// The settings switch. Turning it off clears every persisted credential but
  /// leaves the in-memory ones alone, so the open tab can still reconnect.
  setRememberDevice(enabled: boolean): void {
    const wasEnabled = this.remember
    this.remember = enabled
    if (!enabled) {
      this.clearPersisted()
    } else if (!wasEnabled) {
      this.persist()
    }
  }

  /// Selects localStorage persistence (a plain browser).
  useLocalStorage(): void {
    if (this.backend === 'local') return
    this.backend = 'local'
    this.external = null
    this.memory.clear()
    if (this.storage !== null) this.loadLocal()
  }

  /// Selects the core's `ui.json` persistence (native shell or local UI).
  /// `save` writes the complete map; the caller merges it into the rest of the
  /// preferences document. Credentials that were already in memory (a popout
  /// that connected before boot finished, or a previous local-browser run)
  /// migrate once: they are written to the file store and the localStorage
  /// copies are removed, so the two never disagree.
  useExternalPersistence(save: (records: SessionRecords) => void): void {
    if (this.backend === 'external') {
      this.external = { save }
      return
    }
    const carried = this.snapshot()
    this.backend = 'external'
    this.external = { save }
    if (this.storage !== null) this.writeLocal({})
    if (Object.keys(carried).length > 0 && this.remember) save(carried)
  }

  private prune(): void {
    const now = this.clock()
    let changed = false
    for (const [key, record] of this.memory) {
      if (isExpired(record, now)) {
        this.memory.delete(key)
        changed = true
      }
    }
    if (changed) this.persist()
  }

  private persist(): void {
    if (!this.remember) return
    const records = this.snapshot()
    if (this.backend === 'local') {
      this.writeLocal(records)
    } else if (this.backend === 'external') {
      this.external?.save(records)
    }
  }

  /// Writes a map even while `remember` is off. Only the explicit clear path
  /// uses this.
  private clearPersisted(): void {
    if (this.backend === 'local') {
      this.writeLocal({})
    } else if (this.backend === 'external') {
      this.external?.save({})
    }
  }

  private loadLocal(): void {
    const storage = this.storage
    if (storage === null) return
    const now = this.clock()
    try {
      for (let i = storage.length - 1; i >= 0; i -= 1) {
        const key = storage.key(i)
        if (key === null || !key.startsWith(kSessionKeyPrefix)) continue
        const raw = storage.getItem(key)
        if (raw === null) continue
        try {
          const parsed: unknown = JSON.parse(raw)
          if (!isRecord(parsed) || isExpired(parsed, now)) {
            storage.removeItem(key)
            continue
          }
          this.memory.set(key, { token: parsed.token, savedAt: parsed.savedAt })
        } catch {
          // A corrupt entry is not a credential; drop it.
          storage.removeItem(key)
        }
      }
    } catch {
      // Storage can disappear mid-read; memory keeps what it already has.
    }
  }

  private writeLocal(records: SessionRecords): void {
    const storage = this.storage
    if (storage === null) return
    try {
      const keep = new Set(Object.keys(records))
      // Remove stale keys first; the map is the complete credential set.
      for (let i = storage.length - 1; i >= 0; i -= 1) {
        const key = storage.key(i)
        if (key !== null && key.startsWith(kSessionKeyPrefix) && !keep.has(key)) {
          storage.removeItem(key)
        }
      }
      for (const [key, record] of Object.entries(records)) {
        storage.setItem(key, JSON.stringify(record))
      }
    } catch {
      // Quota/private mode: the in-memory credential still works this run.
    }
  }
}

/// The shared store used by the connection code.
export const sessionStore = new SessionStore()

export function sessionTokenFor(host: string, port: number): string {
  return sessionStore.tokenFor(host, port)
}

export function rememberSession(
  host: string,
  port: number,
  token: string,
  savedAt?: number,
): void {
  sessionStore.rememberToken(host, port, token, savedAt)
}

export function forgetSession(host: string, port: number): void {
  sessionStore.forgetToken(host, port)
}
