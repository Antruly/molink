/// Capability and size rules for the host's delayed file clipboard.
///
/// The host advertises `clipboard_lazy` in its welcome. When it is true the
/// page may send descriptors only and let the host fetch each file on the
/// first remote paste. Hosts without it (older cores, Linux, macOS) keep the
/// upload-then-set path.
export const DEFAULT_LAZY_FILES = 8
export const DEFAULT_LAZY_BYTES = 2 * 1024 * 1024 * 1024
/// The local core reads one clipboard file into memory per request, so a
/// single file is capped well below the host's total budget.
export const MAX_CLIPBOARD_FILE_BYTES = 256 * 1024 * 1024
/// The eager fallback fetches the whole selection into memory before
/// uploading; above this it stays in the files panel.
export const MAX_DIRECT_TOTAL_BYTES = 256 * 1024 * 1024
/// Where the host materialises a delayed offer. Nothing is written until the
/// remote user pastes.
export const LAZY_DEST = '~/moLink/clipboard-lazy'

/// Largest range one virtual-file fetch may ask for. The host reads 64 KiB
/// blocks; this only bounds a malformed or hostile request.
export const MAX_CLIPBOARD_RANGE = 1024 * 1024

export interface LazyCapability {
  maxFiles: number
  maxBytes: number
  maxFileBytes: number
  /// Both sides can stream byte ranges: the host advertises
  /// `clipboard_stream` only when this viewer asked for it in hello.
  stream: boolean
}

function positiveInteger(value: unknown): number | null {
  return typeof value === 'number' && Number.isSafeInteger(value) && value > 0
    ? value
    : null
}

export function lazyCapability(
  welcome: {
    clipboard_lazy?: unknown
    clipboard_lazy_files?: unknown
    clipboard_lazy_bytes?: unknown
    clipboard_stream?: unknown
    clipboard_nested_rdp?: unknown
  } | null | undefined,
): LazyCapability | null {
  if (!welcome || welcome.clipboard_lazy !== true) return null
  const maxFiles = positiveInteger(welcome.clipboard_lazy_files) ??
    DEFAULT_LAZY_FILES
  const maxBytes = positiveInteger(welcome.clipboard_lazy_bytes) ??
    DEFAULT_LAZY_BYTES
  return {
    maxFiles,
    maxBytes,
    maxFileBytes: Math.min(MAX_CLIPBOARD_FILE_BYTES, maxBytes),
    stream: welcome.clipboard_stream === true,
  }
}

export interface ClipboardSize {
  size: number
}

/// True when a local selection can be offered lazily instead of uploaded now.
export function lazyOfferEligible(
  files: readonly ClipboardSize[],
  capability: LazyCapability | null,
): boolean {
  if (capability === null || files.length === 0) return false
  if (files.length > capability.maxFiles) return false
  let total = 0
  for (const file of files) {
    if (!Number.isSafeInteger(file.size) || file.size < 0) return false
    if (file.size > capability.maxFileBytes) return false
    total += file.size
    if (total > capability.maxBytes) return false
  }
  return true
}

/// The eager path needs the whole selection in memory. This also covers the
/// host-side 256 MiB daily limit the file panel already uses.
export function directUploadEligible(
  files: readonly ClipboardSize[],
): boolean {
  if (files.length === 0) return false
  let total = 0
  for (const file of files) {
    if (!Number.isSafeInteger(file.size) || file.size < 0) return false
    if (file.size > MAX_CLIPBOARD_FILE_BYTES) return false
    total += file.size
    if (total > MAX_DIRECT_TOTAL_BYTES) return false
  }
  return true
}

/// How a local selection reaches the remote clipboard.
///
/// `stream` and `lazy` both send descriptors only, so nothing is uploaded at
/// copy time. `stream` additionally answers the host's range fetches instead
/// of uploading a whole file on demand; `direct` is the old eager upload.
export type ClipboardFileMode = 'stream' | 'lazy' | 'direct' | 'none'

export function clipboardFileMode(
  files: readonly ClipboardSize[],
  capability: LazyCapability | null,
): ClipboardFileMode {
  if (lazyOfferEligible(files, capability)) {
    return capability?.stream === true ? 'stream' : 'lazy'
  }
  if (directUploadEligible(files)) return 'direct'
  return 'none'
}

/// One file of the offer, as the page remembers it.
export interface OfferedClipboardFile {
  id?: string
  name: string
  size: number
}

export interface ClipboardFetchTarget {
  fileIndex: number
  fileId: string
  offset: number
  length: number
  /// Size the offer advertised; the reply must cover it exactly or report an
  /// error, so a short local read cannot silently truncate the pasted file.
  size: number
}

/// A `fetch` either names a readable range or must be answered with an error.
export type ClipboardFetchResolution =
  | { ok: true; id: number; target: ClipboardFetchTarget }
  | { ok: false; id: number; offset: number; error: number }

/// Maps a host `fetch` message (`id`, optional `index`, `name`, `offset`,
/// `length`) onto the offered file it belongs to.
///
/// `id` here is the host's range-request id, not the local clipboard id. The
/// index is preferred because two folders can contribute the same base name;
/// the name is a cross-check against a stale message from a retired offer.
export function resolveClipboardFetch(
  message: {
    id?: unknown
    index?: unknown
    name?: unknown
    offset?: unknown
    length?: unknown
  },
  files: readonly OfferedClipboardFile[],
): ClipboardFetchResolution {
  const id =
    typeof message.id === 'number' && Number.isSafeInteger(message.id) &&
    message.id > 0
      ? message.id
      : 0
  const offset =
    typeof message.offset === 'number' &&
    Number.isSafeInteger(message.offset) && message.offset >= 0
      ? message.offset
      : 0
  const length =
    typeof message.length === 'number' && Number.isSafeInteger(message.length)
      ? message.length
      : 0
  const fail = (): ClipboardFetchResolution => ({
    ok: false,
    id,
    offset,
    error: 1,
  })
  if (id === 0) return fail()
  if (length <= 0 || length > MAX_CLIPBOARD_RANGE) return fail()

  let index = -1
  if (
    typeof message.index === 'number' &&
    Number.isInteger(message.index) &&
    message.index >= 0 &&
    message.index < files.length
  ) {
    index = message.index
  } else if (typeof message.name === 'string') {
    index = files.findIndex((file) => file.name === message.name)
  }
  if (index < 0) return fail()
  const file = files[index]
  if (!file || !file.id) return fail()
  // A fetch for a different name under this index is a message from an offer
  // this page has already replaced.
  if (typeof message.name === 'string' && message.name !== file.name) {
    return fail()
  }
  if (offset > file.size) return fail()
  return {
    ok: true,
    id,
    target: {
      fileIndex: index,
      fileId: file.id,
      offset,
      length,
      size: file.size,
    },
  }
}

/// Query suffix for a range read through the local core's
/// `/api/clipboard-file` route. Without a range it is empty (whole file).
export function clipboardRangeQuery(
  offset?: number,
  length?: number,
): string {
  const params = new URLSearchParams()
  if (offset !== undefined) params.set('offset', String(offset))
  if (length !== undefined) params.set('length', String(length))
  const text = params.toString()
  return text === '' ? '' : `&${text}`
}
