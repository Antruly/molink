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

export interface LazyCapability {
  maxFiles: number
  maxBytes: number
  maxFileBytes: number
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
