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
export const MAX_DIAG_ENTRIES = 600
const MAX_STORAGE_BYTES = 256 * 1024
const MAX_MSG_LENGTH = 500
const MAX_VALUE_LENGTH = 300
const MAX_DATA_KEYS = 16
const SAVE_DELAY_MS = 400

let entries: DiagEntry[] = readStored()
let saveTimer: number | null = null
let captureInstalled = false

const listeners = new Set<() => void>()

function truncate(value: string, limit: number): string {
  const oneLine = value.replace(/\s+/g, ' ').trim()
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
  if (typeof window === 'undefined') return []
  try {
    const raw = window.localStorage.getItem(STORAGE_KEY)
    if (raw === null || raw === '') return []
    const parsed: unknown = JSON.parse(raw)
    if (!Array.isArray(parsed)) return []
    const loaded = parsed
      .map(normalizeStored)
      .filter((entry): entry is DiagEntry => entry !== null)
    return loaded.slice(-MAX_DIAG_ENTRIES)
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
  if (typeof window === 'undefined') return
  let stored = entries
  let text = JSON.stringify(stored)
  while (byteLength(text) > MAX_STORAGE_BYTES && stored.length > 1) {
    stored = stored.slice(Math.ceil(stored.length / 10))
    text = JSON.stringify(stored)
  }
  try {
    window.localStorage.setItem(STORAGE_KEY, text)
  } catch {
    // Quota or blocked storage: keep logging in memory only.
  }
}

function scheduleSave(): void {
  if (typeof window === 'undefined') return
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
  if (typeof window !== 'undefined') {
    try {
      window.localStorage.removeItem(STORAGE_KEY)
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
