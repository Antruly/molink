// Wire format for the host's `/ws/terminal` endpoint.
//
// Text frames carry JSON control messages; terminal bytes travel in binary
// frames with a small binary header so a paste never becomes base64:
//
//   output (host -> viewer): [0x01][u64 id][u64 seq][payload]
//   input  (viewer -> host): [0x02][u64 id][payload]
//
// `seq` is the total number of bytes the host has written to this terminal, so
// the frame's number is also the sequence number of its first payload byte.
// A viewer attaches with the number of bytes it has already rendered and gets
// everything from there (the host may start later when its 1 MiB ring already
// evicted the requested offset).

export const TERM_FRAME_OUTPUT = 0x01
export const TERM_FRAME_INPUT = 0x02
export const TERM_OUTPUT_HEADER_SIZE = 1 + 8 + 8
export const TERM_INPUT_HEADER_SIZE = 1 + 8

export type TerminalControl =
  | { type: 'term-open'; cols: number; rows: number; shell?: string }
  | { type: 'term-opened'; id: number }
  | { type: 'term-attach'; id: number; seq: number }
  | { type: 'term-resize'; id: number; cols: number; rows: number }
  | { type: 'term-close'; id: number }
  | { type: 'term-exit'; id: number; code: number }
  | { type: 'error'; code: string; message?: string }

export interface TerminalOutputFrame {
  id: number
  seq: number
  data: Uint8Array
}

function asBytes(input: ArrayBuffer | ArrayBufferView): Uint8Array {
  if (input instanceof Uint8Array) return input
  if (ArrayBuffer.isView(input)) {
    return new Uint8Array(input.buffer, input.byteOffset, input.byteLength)
  }
  return new Uint8Array(input)
}

function putU64(view: DataView, offset: number, value: number): void {
  view.setBigUint64(offset, BigInt(Math.trunc(value)), true)
}

function getU64(view: DataView, offset: number): number {
  return Number(view.getBigUint64(offset, true))
}

// ------------------------------------------------------------- control text

export function terminalOpen(
  cols: number,
  rows: number,
  shell?: string,
): TerminalControl {
  const message: TerminalControl = { type: 'term-open', cols, rows }
  if (shell !== undefined && shell !== '') message.shell = shell
  return message
}

export function terminalAttach(id: number, seq: number): TerminalControl {
  return { type: 'term-attach', id, seq }
}

export function terminalResize(
  id: number,
  cols: number,
  rows: number,
): TerminalControl {
  return { type: 'term-resize', id, cols, rows }
}

export function terminalClose(id: number): TerminalControl {
  return { type: 'term-close', id }
}

export function encodeControl(message: TerminalControl): string {
  return JSON.stringify(message)
}

function isInteger(value: unknown): value is number {
  return typeof value === 'number' && Number.isInteger(value)
}

/// Parse a control message. `null` means "not a terminal message this viewer
/// understands" (a newer host may add fields; unknown types are ignored).
export function decodeControl(text: string): TerminalControl | null {
  let value: unknown
  try {
    value = JSON.parse(text)
  } catch {
    return null
  }
  if (value === null || typeof value !== 'object') return null
  const message = value as Record<string, unknown>
  const type = message.type
  switch (type) {
    case 'term-open':
      if (!isInteger(message.cols) || !isInteger(message.rows)) return null
      return terminalOpen(
        message.cols,
        message.rows,
        typeof message.shell === 'string' ? message.shell : undefined,
      )
    case 'term-opened':
      if (!isInteger(message.id)) return null
      return { type, id: message.id }
    case 'term-attach':
      if (!isInteger(message.id) || !isInteger(message.seq)) return null
      return { type, id: message.id, seq: message.seq }
    case 'term-resize':
      if (
        !isInteger(message.id) ||
        !isInteger(message.cols) ||
        !isInteger(message.rows)
      ) {
        return null
      }
      return {
        type,
        id: message.id,
        cols: message.cols,
        rows: message.rows,
      }
    case 'term-close':
      if (!isInteger(message.id)) return null
      return { type, id: message.id }
    case 'term-exit':
      if (!isInteger(message.id) || !isInteger(message.code)) return null
      return { type, id: message.id, code: message.code }
    case 'error': {
      if (typeof message.code !== 'string') return null
      const error: TerminalControl = { type, code: message.code }
      if (typeof message.message === 'string') error.message = message.message
      return error
    }
    default:
      return null
  }
}

// ------------------------------------------------------------- binary frames

/// Viewer -> host keystrokes. `data` is UTF-8 bytes from xterm's onData.
export function encodeInputFrame(id: number, data: Uint8Array): Uint8Array {
  const frame = new Uint8Array(TERM_INPUT_HEADER_SIZE + data.length)
  frame[0] = TERM_FRAME_INPUT
  putU64(new DataView(frame.buffer), 1, id)
  frame.set(data, TERM_INPUT_HEADER_SIZE)
  return frame
}

/// Host -> viewer output. The frontend only decodes these, but encoding keeps
/// the round trip testable and documents the header in one place.
export function encodeOutputFrame(
  id: number,
  seq: number,
  data: Uint8Array,
): Uint8Array {
  const frame = new Uint8Array(TERM_OUTPUT_HEADER_SIZE + data.length)
  frame[0] = TERM_FRAME_OUTPUT
  const view = new DataView(frame.buffer)
  putU64(view, 1, id)
  putU64(view, 9, seq)
  frame.set(data, TERM_OUTPUT_HEADER_SIZE)
  return frame
}

export function decodeOutputFrame(
  buffer: ArrayBuffer | ArrayBufferView,
): TerminalOutputFrame | null {
  const bytes = asBytes(buffer)
  if (
    bytes.length < TERM_OUTPUT_HEADER_SIZE ||
    bytes[0] !== TERM_FRAME_OUTPUT
  ) {
    return null
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
  return {
    id: getU64(view, 1),
    seq: getU64(view, 9),
    data: bytes.slice(TERM_OUTPUT_HEADER_SIZE),
  }
}

// ------------------------------------------------------------ replay dedupe

export interface DedupedOutput {
  /// Sequence number of the next byte the viewer still needs (bytes rendered
  /// so far, matching what `term-attach` must send).
  seq: number
  data: Uint8Array
}

/// Fold one output frame into the viewer's byte counter.
///
/// `lastSeq` is the number of bytes already rendered. A replayed frame may
/// overlap what the page shows (attach replays from the last byte the page
/// saw, and a live frame can race that replay), so only the unseen tail is
/// returned. Fully old frames return `null`.
export function applyOutputFrame(
  lastSeq: number,
  frame: { seq: number; data: Uint8Array },
): DedupedOutput | null {
  const length = frame.data.length
  if (length === 0) return null
  const end = frame.seq + length
  if (end <= lastSeq) return null
  const offset = lastSeq > frame.seq ? lastSeq - frame.seq : 0
  const data = offset === 0 ? frame.data : frame.data.slice(offset)
  return { seq: Math.max(lastSeq, end), data }
}

// ----------------------------------------------------------- resize coalesce

export interface TimerApi {
  setTimeout(callback: () => void, delay: number): unknown
  clearTimeout(handle: unknown): void
}

export interface ResizeDebouncer {
  /// Record the size the terminal is now showing. The host only hears the last
  /// one once the user stops dragging (default 200 ms).
  update(cols: number, rows: number): void
  /// Send the pending size immediately (e.g. before a resize causes the host
  /// to redraw something that depends on the geometry).
  flush(): void
  cancel(): void
  pending(): { cols: number; rows: number } | null
}

const defaultTimers: TimerApi = {
  setTimeout: (callback, delay) => globalThis.setTimeout(callback, delay),
  clearTimeout: (handle) =>
    globalThis.clearTimeout(handle as ReturnType<typeof setTimeout>),
}

export function createResizeDebouncer(
  send: (cols: number, rows: number) => void,
  options: { delay?: number; timers?: TimerApi } = {},
): ResizeDebouncer {
  const delay = options.delay ?? 200
  const timers = options.timers ?? defaultTimers
  let timer: unknown = null
  let pendingSize: { cols: number; rows: number } | null = null
  let lastSent: { cols: number; rows: number } | null = null

  const stop = () => {
    if (timer !== null) {
      timers.clearTimeout(timer)
      timer = null
    }
  }

  const flush = () => {
    stop()
    const size = pendingSize
    pendingSize = null
    if (size === null) return
    if (lastSent !== null && lastSent.cols === size.cols &&
        lastSent.rows === size.rows) {
      return
    }
    lastSent = size
    send(size.cols, size.rows)
  }

  return {
    update(cols, rows) {
      if (cols <= 0 || rows <= 0) return
      pendingSize = { cols, rows }
      stop()
      // A size that matches what the host already has needs no message; drop
      // a timer left over from the intermediate value.
      if (lastSent !== null && lastSent.cols === cols && lastSent.rows === rows) {
        pendingSize = null
        return
      }
      timer = timers.setTimeout(() => {
        timer = null
        flush()
      }, delay)
    },
    flush() {
      flush()
    },
    cancel() {
      stop()
      pendingSize = null
    },
    pending() {
      return pendingSize
    },
  }
}

// --------------------------------------------------------------------- URL

/// The terminal WebSocket lives on the host core, next to `/ws/session`. A page
/// served over HTTPS may only open wss, so the scheme follows the page.
export function terminalSocketUrl(
  host: string,
  port: number,
  pageProtocol: string,
  token: string,
): string {
  const scheme = pageProtocol === 'https:' ? 'wss' : 'ws'
  const trimmed = host.trim()
  const authority = trimmed.startsWith('[')
    ? trimmed
    : trimmed.includes(':')
      ? `[${trimmed}]`
      : trimmed
  const query = new URLSearchParams({ token })
  return `${scheme}://${authority}:${port}/ws/terminal?${query.toString()}`
}
