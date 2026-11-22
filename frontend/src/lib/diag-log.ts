/// On-device diagnostics: a small, bounded ring buffer of connection and
/// decode events, kept in localStorage so a transient banner can still be
/// traced after the fact. The buffer is deliberately quiet: it never uploads
/// anything, never logs tokens or clipboard content, and is only reachable
/// from a low-contrast entry at the bottom of the settings dialog.
///
/// The web GUI runs inside a WebView with no reachable console (Windows
/// shell, native GTK shell), so this is the only local record a user can
/// export when a remote session shows an error such as
/// "TypeError: Failed to Decode Data.".

export type DiagLevel = 'info' | 'warn' | 'error'

export type DiagValue = string | number | boolean | null

export type DiagData = Record<string, DiagValue | undefined>

export interface DiagEntry {
  /** Epoch milliseconds. */
  t: number
  lvl: DiagLevel
  /** Stable machine-ish event name, e.g. "frame.inflate" or "ws.close". */
  ev: string
  /** Optional human-readable detail. */
  msg?: string
  data?: DiagData
}

export interface DiagExportOptions {
  /** Core/app version, shown in the export header. */
  version?: string
  /** Entries to export; defaults to the whole buffer. */
  entries?: readonly DiagEntry[]
  /** Extra note such as the active filter. */
  note?: string
}

const STORAGE_KEY = 'molink.diag.v1'
const ENABLED_KEY = 'molink.diag.enabled.v1'
/// Entries older than this are pruned on load and on every save. The host
/// keeps its own files for the same seven days.
export const DIAG_RETENTION_MS = 7 * 24 * 60 * 60 * 1000
export const MAX_DIAG_ENTRIES = 600
const MAX_STORAGE_BYTES = 256 * 1024
const MAX_MSG_LENGTH = 500
const MAX_VALUE_LENGTH = 300
const MAX_DATA_KEYS = 16
const SAVE_DELAY_MS = 400

/// The subset of Storage this module needs; tests can supply a fake.
export interface DiagStorage {
  getItem(key: string): string | null
  setItem(key: string, value: string): void
  removeItem(key: string): void
}

let storageOverride: DiagStorage | null = null
let recordingEnabled = readEnabled()
let entries: DiagEntry[] = readStored()
let saveTimer: number | null = null
let captureInstalled = false

const listeners = new Set<() => void>()

function activeStorage(): DiagStorage | null {
  if (storageOverride !== null) return storageOverride
  if (typeof window === 'undefined') return null
  try {
    return window.localStorage
  } catch {
    return null
  }
}

function readEnabled(store: DiagStorage | null = activeStorage()): boolean {
  if (store === null) return true
  try {
    // Default on: only an explicit '0' switches recording off.
    return store.getItem(ENABLED_KEY) !== '0'
  } catch {
    return true
  }
}

/// Whether diagnostics are being written to local storage. The in-memory
/// ring keeps recording either way; the host has its own switch.
export function isDiagRecordingEnabled(store?: DiagStorage): boolean {
  return store !== undefined ? readEnabled(store) : recordingEnabled
}

/// Persists the recording switch. Turning it off never deletes what is
/// already stored; only an explicit "clear" does that.
export function setDiagRecordingEnabled(
  enabled: boolean,
  store?: DiagStorage,
): void {
  recordingEnabled = enabled
  const target = store ?? activeStorage()
  try {
    target?.setItem(ENABLED_KEY, enabled ? '1' : '0')
  } catch {
    // Quota or blocked storage: the in-memory setting still applies.
  }
  notify()
}

/// Test hook: replace the storage backend (pass null to restore the window).
export function __setDiagStorageForTest(store: DiagStorage | null): void {
  storageOverride = store
  recordingEnabled = readEnabled(store)
  entries = readStored()
  notify()
}

/// Drops entries outside the retention window. Exported for the unit tests.
export function pruneDiagEntries(
  list: readonly DiagEntry[],
  now: number,
  maxAgeMs = DIAG_RETENTION_MS,
): DiagEntry[] {
  return list.filter((entry) => now - entry.t <= maxAgeMs)
}

/// Credentials must not reach the ring buffer or local storage. The host
/// redacts its own lines; the page applies the same rules to messages and
/// field values before they are stored. Error names such as `unauthorized`
/// are deliberately left alone -- only labelled values and token-shaped runs
/// are masked.
export function redactDiagnosticText(value: string): string {
  // "unknown key code: MetaLeft" is the one diagnostic whose whole point is
  // the value after the colon; hide the phrase from the code rule.
  const guard = '\u0000KEYCODE\u0000'
  let text = value.replace(/key\s+code/gi, guard)
  text = text
    .replace(/\b(token|session|code|password|passwd|secret)\s*[=:]\s*\S+/gi,
      '$1=***')
    .replace(/\b[0-9a-fA-F]{32,}\b/g, '***')
    .replace(/\b[A-Z0-9]{3}-[A-Z0-9]{3}-[A-Z0-9]{3}\b/g, '***')
  return text.split(guard).join('key code')
}

function truncate(value: string, limit: number): string {
  const oneLine = redactDiagnosticText(value).replace(/\s+/g, ' ').trim()
  return oneLine.length > limit ? `${oneLine.slice(0, limit)}…` : oneLine
}

function normalizeValue(value: unknown): DiagValue | undefined {
  if (value === undefined) return undefined
  if (value === null) return null
  if (typeof value === 'number') {
    return Number.isFinite(value) ? value : String(value)
  }
  if (typeof value === 'boolean') return value
  if (typeof value === 'string') return truncate(value, MAX_VALUE_LENGTH)
  try {
    return truncate(JSON.stringify(value) ?? String(value), MAX_VALUE_LENGTH)
  } catch {
    return String(value)
  }
}

function normalizeData(data: DiagData | undefined): DiagData | undefined {
  if (!data) return undefined
  const out: DiagData = {}
  let keys = 0
  for (const [key, value] of Object.entries(data)) {
    const normalized = normalizeValue(value)
    if (normalized === undefined) continue
    out[truncate(key, 40)] = normalized
    keys += 1
    if (keys >= MAX_DATA_KEYS) break
  }
  return keys > 0 ? out : undefined
}

function normalizeStored(raw: unknown): DiagEntry | null {
  if (raw === null || typeof raw !== 'object') return null
  const item = raw as Record<string, unknown>
  if (typeof item.t !== 'number' || typeof item.ev !== 'string') return null
  const lvl: DiagLevel =
    item.lvl === 'warn' || item.lvl === 'error' ? item.lvl : 'info'
  const entry: DiagEntry = {
    t: item.t,
    lvl,
    ev: truncate(item.ev, 60),
  }
  if (typeof item.msg === 'string' && item.msg !== '') {
    entry.msg = truncate(item.msg, MAX_MSG_LENGTH)
  }
  if (item.data !== null && typeof item.data === 'object') {
    const data = normalizeData(item.data as DiagData)
    if (data) entry.data = data
  }
  return entry
}

function readStored(): DiagEntry[] {
  const store = activeStorage()
  if (store === null) return []
  try {
    const raw = store.getItem(STORAGE_KEY)
    if (raw === null || raw === '') return []
    const parsed: unknown = JSON.parse(raw)
    if (!Array.isArray(parsed)) return []
    const loaded = parsed
      .map(normalizeStored)
      .filter((entry): entry is DiagEntry => entry !== null)
    return pruneDiagEntries(loaded, Date.now()).slice(-MAX_DIAG_ENTRIES)
  } catch {
    return []
  }
}

function notify(): void {
  for (const listener of listeners) {
    try {
      listener()
    } catch {
      // A broken listener must not take the logging path down.
    }
  }
}

function byteLength(text: string): number {
  if (typeof TextEncoder !== 'undefined') return new TextEncoder().encode(text).length
  return text.length
}

/// Write the buffer now, trimming the persisted copy if it grew past the
/// storage budget. In-memory entries stay intact for the viewer.
export function flushDiag(): void {
  if (saveTimer !== null && typeof window !== 'undefined') {
    window.clearTimeout(saveTimer)
    saveTimer = null
  }
  const store = activeStorage()
  // Recording off means memory only: no file is created and no write happens.
  if (store === null || !recordingEnabled) return
  let stored = pruneDiagEntries(entries, Date.now())
  let text = JSON.stringify(stored)
  while (byteLength(text) > MAX_STORAGE_BYTES && stored.length > 1) {
    stored = stored.slice(Math.ceil(stored.length / 10))
    text = JSON.stringify(stored)
  }
  try {
    store.setItem(STORAGE_KEY, text)
  } catch {
    // Quota or blocked storage: keep logging in memory only.
  }
}

function scheduleSave(): void {
  if (typeof window === 'undefined' || !recordingEnabled) return
  if (saveTimer !== null) return
  saveTimer = window.setTimeout(() => {
    saveTimer = null
    flushDiag()
  }, SAVE_DELAY_MS)
}

/// Record one diagnostic event. Safe to call on a hot path: entries are
/// appended to a bounded array and persisted at most a few times a second.
export function logDiag(
  level: DiagLevel,
  event: string,
  message?: string | null,
  data?: DiagData,
): void {
  try {
    const entry: DiagEntry = {
      t: Date.now(),
      lvl: level,
      ev: truncate(event, 60),
    }
    if (message !== undefined && message !== null && message !== '') {
      entry.msg = truncate(String(message), MAX_MSG_LENGTH)
    }
    const normalized = normalizeData(data)
    if (normalized) entry.data = normalized

    const next = entries.slice()
    next.push(entry)
    entries =
      next.length > MAX_DIAG_ENTRIES
        ? next.slice(next.length - MAX_DIAG_ENTRIES)
        : next
    scheduleSave()
    notify()
  } catch {
    // Diagnostics must never break the caller.
  }
}

export function getDiagEntries(): readonly DiagEntry[] {
  return entries
}

export function subscribeDiag(listener: () => void): () => void {
  listeners.add(listener)
  return () => {
    listeners.delete(listener)
  }
}

export function clearDiag(): void {
  entries = []
  if (saveTimer !== null && typeof window !== 'undefined') {
    window.clearTimeout(saveTimer)
    saveTimer = null
  }
  const store = activeStorage()
  if (store !== null) {
    try {
      store.removeItem(STORAGE_KEY)
    } catch {
      // ignore
    }
  }
  notify()
}

function pad(value: number, width = 2): string {
  return String(value).padStart(width, '0')
}

export function formatDiagTime(t: number): string {
  const date = new Date(t)
  return (
    `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())} ` +
    `${pad(date.getHours())}:${pad(date.getMinutes())}:` +
    `${pad(date.getSeconds())}.${pad(date.getMilliseconds(), 3)}`
  )
}

function formatValue(value: DiagValue): string {
  if (value === null) return 'null'
  if (typeof value === 'string') {
    return /[\s"=]/.test(value) ? JSON.stringify(value) : value
  }
  return String(value)
}

export function formatDiagLine(entry: DiagEntry): string {
  const head = `${formatDiagTime(entry.t)} ${entry.lvl.toUpperCase().padEnd(5)} ${entry.ev}`
  const fields = entry.data
    ? Object.entries(entry.data)
        .filter(([, value]) => value !== undefined)
        .map(([key, value]) => `${key}=${formatValue(value as DiagValue)}`)
        .join(' ')
    : ''
  const tail = entry.msg ? ` :: ${entry.msg}` : ''
  return fields ? `${head} ${fields}${tail}` : `${head}${tail}`
}

/// Human-readable export used by the settings log viewer. The header carries
/// the trivia a maintainer asks for first (build, page, platform), then one
/// line per event, oldest first.
export function buildDiagText(options: DiagExportOptions = {}): string {
  const selected = options.entries ?? entries
  const errorCount = selected.filter((entry) => entry.lvl === 'error').length
  const warnCount = selected.filter((entry) => entry.lvl === 'warn').length
  const infoCount = selected.length - errorCount - warnCount
  const nav = typeof navigator !== 'undefined' ? navigator : undefined
  const page = typeof window !== 'undefined' ? window.location : undefined
  const header = [
    'moLink diagnostics log',
    `exported: ${new Date().toISOString()}`,
    `version: ${options.version || 'unknown'}`,
    `page: ${page ? `${page.origin}${page.pathname}` : 'unknown'}`,
    `language: ${nav?.language ?? 'unknown'}`,
    `platform: ${nav?.userAgent ?? 'unknown'}`,
    `entries: ${selected.length} of ${entries.length} stored ` +
      `(limit ${MAX_DIAG_ENTRIES}, oldest first)`,
    `counts: error=${errorCount} warn=${warnCount} info=${infoCount}`,
  ]
  if (options.note) header.push(`note: ${options.note}`)
  header.push('---')
  return [...header, ...selected.map(formatDiagLine), ''].join('\n')
}

export interface CopyLogOptions {
  /** This machine's core/shell version. */
  version?: string
  /** The host's version from its welcome, when connected. */
  hostVersion?: string
  /** False when no session is live: the host section says so. */
  connected: boolean
  /** The host's `logs tail` text. */
  hostTail?: string
  /** How many local lines to include (default 200). */
  localLines?: number
}

/// The one-click "copy recent diagnostics" document. It is deliberately
/// plain text so it can be pasted into a chat unchanged, and it opens with
/// the build pair and the exact local time a maintainer asks for first.
/// ISO 8601 with the local offset, e.g. 2026-11-23T06:38:52.536+08:00.
function localIsoString(date = new Date()): string {
  const pad = (value: number, width = 2) => String(value).padStart(width, '0')
  const offset = -date.getTimezoneOffset()
  const sign = offset >= 0 ? '+' : '-'
  const abs = Math.abs(offset)
  return (
    `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())}` +
    `T${pad(date.getHours())}:${pad(date.getMinutes())}:` +
    `${pad(date.getSeconds())}.${pad(date.getMilliseconds(), 3)}` +
    `${sign}${pad(Math.floor(abs / 60))}:${pad(abs % 60)}`
  )
}

export function buildCopyLogText(options: CopyLogOptions): string {
  const localLines = Math.max(1, options.localLines ?? 200)
  const local = entries.slice(-localLines)
  const hostBody = options.connected
    ? (options.hostTail ?? '').replace(/\s+$/, '')
    : ''
  const hostLines =
    options.connected && hostBody !== '' ? hostBody : options.connected
      ? '(no host entries)'
      : 'not connected'
  return [
    '=== moLink diag ===',
    `version: ${options.version || 'unknown'} / host ${
      options.connected ? options.hostVersion || 'unknown' : 'unknown'
    }`,
    `time: ${localIsoString()}`,
    '--- local ---',
    ...local.map(formatDiagLine),
    '--- host ---',
    hostLines,
    '',
  ].join('\n')
}

/// Uncaught errors and unhandled rejections are the ones most likely to show
/// up as a transient banner. Repeated messages are collapsed so one bad frame
/// loop cannot flush the buffer.
const uncaughtRepeats = new Map<string, { count: number; at: number }>()

function logUncaught(
  event: string,
  message: string,
  data?: DiagData,
): void {
  const now = Date.now()
  const previous = uncaughtRepeats.get(message)
  if (previous !== undefined && now - previous.at < 5000) {
    previous.count += 1
    return
  }
  const repeated = previous?.count ?? 0
  // A page that produces a new message for every tick must not turn this
  // dedupe table into a leak.
  if (uncaughtRepeats.size > 50) uncaughtRepeats.clear()
  uncaughtRepeats.set(message, { count: 0, at: now })
  logDiag('error', event, message, {
    ...data,
    ...(repeated > 0 ? { repeat: repeated } : {}),
  })
}

function installGlobalCapture(): void {
  if (captureInstalled || typeof window === 'undefined') return
  captureInstalled = true

  window.addEventListener('error', (event) => {
    const message = event.message || 'uncaught error'
    // WebKit emits this for benign layout loops; it is not actionable.
    if (/ResizeObserver loop/i.test(message)) return
    logUncaught('window.error', message, {
      where: event.filename
        ? `${event.filename}:${event.lineno}:${event.colno}`
        : undefined,
    })
  })

  window.addEventListener('unhandledrejection', (event) => {
    const reason: unknown = event.reason
    const message =
      reason instanceof Error
        ? reason.message || reason.name
        : typeof reason === 'string'
          ? reason
          : 'unhandled rejection'
    const stack =
      reason instanceof Error && reason.stack
        ? reason.stack.split('\n')[1]?.trim()
        : undefined
    logUncaught('promise.rejection', message, { where: stack })
  })

  window.addEventListener('pagehide', flushDiag)
}

installGlobalCapture()
