/// Dragging remote entries out of the session window.
///
/// A browser cannot start an OS-level drag from bytes it has not fetched yet,
/// so the page does the only thing it can: it carries the remote paths during
/// the HTML drag, detects that the pointer left the window, and hands the
/// payload to the desktop shell. The shell prepares local copies and starts a
/// real `text/uri-list` drag. A plain browser falls back to downloading the
/// selection instead. Keep the payload and the gesture rules here so both the
/// panel and its tests agree on them.

export const REMOTE_DRAG_MIME = 'application/x-molink-remote'

export interface RemoteDragItem {
  /// Absolute path on the host.
  path: string
  name: string
  dir: boolean
  /// Listing metadata, not part of the bridge payload: drag-out uses it for
  /// the progress total before the recursive walk reports exact sizes.
  size?: number
}

export interface RemoteDragPayload {
  paths: string[]
  names: string[]
}

/// What `dragstart` puts on the DataTransfer. Paths are what the shell needs;
/// names make the drag icon and the fallback download readable.
export function buildRemoteDragPayload(
  items: RemoteDragItem[],
): RemoteDragPayload {
  return {
    paths: items.map((item) => item.path),
    names: items.map((item) => item.name),
  }
}

export function encodeRemoteDragPayload(payload: RemoteDragPayload): string {
  return JSON.stringify(payload)
}

export function parseRemoteDragPayload(
  raw: string | null | undefined,
): RemoteDragPayload | null {
  if (!raw) return null
  try {
    const value = JSON.parse(raw) as Partial<RemoteDragPayload>
    if (!Array.isArray(value.paths) || !Array.isArray(value.names)) return null
    const paths = value.paths.filter(
      (path): path is string => typeof path === 'string' && path !== '',
    )
    if (paths.length === 0) return null
    const names = value.names.map((name, index) =>
      typeof name === 'string' && name !== ''
        ? name
        : paths[index]?.split(/[\\/]/).pop() ?? 'file',
    )
    return { paths, names: names.slice(0, paths.length) }
  } catch {
    return null
  }
}

/// The row the user started dragging: an already-selected row drags the whole
/// selection, an unselected row drags only itself. This matches file managers.
export function dragItemsFor(
  entry: RemoteDragItem,
  selected: RemoteDragItem[],
): RemoteDragItem[] {
  if (selected.some((item) => item.path === entry.path)) return selected
  return [entry]
}

/// One pixel of slack: WebKit reports the edge coordinate as exactly the
/// window width or height when the pointer leaves, and a fractional device
/// pixel would otherwise count as "inside".
export function isOutsideWindow(
  x: number,
  y: number,
  width: number,
  height: number,
): boolean {
  return x <= 0 || y <= 0 || x >= width - 1 || y >= height - 1
}

/// A drag that ended with `none` outside the window is the user asking for a
/// file on the desktop. A drop the page accepted (`copy`) is not.
export function dragEndedOutside(
  dropEffect: string | null | undefined,
  leftWindow: boolean,
  x: number,
  y: number,
  width: number,
  height: number,
): boolean {
  if (dropEffect === 'copy' || dropEffect === 'move' || dropEffect === 'link') {
    return false
  }
  return leftWindow || isOutsideWindow(x, y, width, height)
}
