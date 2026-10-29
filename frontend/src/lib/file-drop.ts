/// Dropped files, including folder drags, staged for the transfer panel.
///
/// A drop on the session belongs to the file-transfer drawer, not to the
/// clipboard-paste path: the user gets a list, a destination they can change
/// and an explicit "send". This module is the hand-off between the session
/// surface (which sees the drop) and the drawer (which owns the list).

export interface StagedFile {
  file: File
  /// Path inside the drop, e.g. `photos/2024/a.jpg`, or just the file name.
  path: string
}

type Listener = (files: StagedFile[]) => void

const listeners = new Set<Listener>()

export function onDroppedFiles(listener: Listener): () => void {
  listeners.add(listener)
  return () => {
    listeners.delete(listener)
  }
}

export function stageDroppedFiles(files: StagedFile[]): void {
  if (files.length === 0) return
  for (const listener of listeners) listener(files)
}

interface FileSystemEntryLike {
  isFile: boolean
  isDirectory: boolean
  name: string
}

interface FileSystemFileEntryLike extends FileSystemEntryLike {
  file(resolve: (file: File) => void, reject: (error: unknown) => void): void
}

interface FileSystemDirectoryEntryLike extends FileSystemEntryLike {
  createReader(): {
    readEntries(
      resolve: (entries: FileSystemEntryLike[]) => void,
      reject: (error: unknown) => void,
    ): void
  }
}

function readAllEntries(reader: ReturnType<
  FileSystemDirectoryEntryLike['createReader']
>): Promise<FileSystemEntryLike[]> {
  return new Promise((resolve, reject) => {
    const all: FileSystemEntryLike[] = []
    const next = () => {
      reader.readEntries((entries) => {
        if (entries.length === 0) {
          resolve(all)
          return
        }
        all.push(...entries)
        next()
      }, reject)
    }
    next()
  })
}

async function walkEntry(
  entry: FileSystemEntryLike,
  prefix: string,
  out: StagedFile[],
): Promise<void> {
  if (entry.isFile) {
    const file = await new Promise<File>((resolve, reject) => {
      ;(entry as FileSystemFileEntryLike).file(resolve, reject)
    })
    out.push({ file, path: prefix + file.name })
    return
  }
  if (entry.isDirectory) {
    const childPrefix = prefix + entry.name + '/'
    const entries = await readAllEntries(
      (entry as FileSystemDirectoryEntryLike).createReader(),
    )
    for (const child of entries) await walkEntry(child, childPrefix, out)
  }
}

/// Files, folders, or a mix, flattened with their relative paths. Browsers
/// without the entries API fall back to the plain file list.
export async function collectDropEntries(
  transfer: DataTransfer,
): Promise<StagedFile[]> {
  const items = Array.from(transfer.items ?? [])
  const entries: FileSystemEntryLike[] = []
  for (const item of items) {
    if (item.kind !== 'file') continue
    const entry = (
      item as DataTransferItem & {
        webkitGetAsEntry?: () => FileSystemEntryLike | null
      }
    ).webkitGetAsEntry?.()
    if (entry) entries.push(entry)
  }
  if (entries.length === 0) {
    return Array.from(transfer.files ?? []).map((file) => ({
      file,
      path: file.name,
    }))
  }
  const out: StagedFile[] = []
  for (const entry of entries) await walkEntry(entry, '', out)
  return out
}
