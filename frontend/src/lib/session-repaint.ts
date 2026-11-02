/**
 * Frame-arrival notifications for one session's back buffer.
 *
 * The connection composites every incoming frame into the off-screen canvas
 * (screen-buffer.ts). Putting that canvas into React state meant a full store
 * update and re-render of the session view at the wire frame rate, which is
 * the main reason the browser shell stuttered on a busy desktop. Instead the
 * canvas subscribes here and blits once per display refresh, while React only
 * sees frame metadata a few times a second.
 */
type Listener = () => void

const listeners = new Map<string, Set<Listener>>()
const versions = new Map<string, number>()

/// A frame was applied to the session's back buffer; ask its canvas to blit.
export function noteSessionFrame(key: string): void {
  if (key === '') return
  versions.set(key, (versions.get(key) ?? 0) + 1)
  const set = listeners.get(key)
  if (set === undefined) return
  for (const listener of set) listener()
}

/// Monotonic counter, useful for "did anything arrive since?" checks.
export function sessionFrameVersion(key: string): number {
  return versions.get(key) ?? 0
}

/// Subscribe to frame arrivals for one session. Returns an unsubscribe.
export function onSessionFrame(key: string, listener: Listener): () => void {
  let set = listeners.get(key)
  if (set === undefined) {
    set = new Set()
    listeners.set(key, set)
  }
  set.add(listener)
  return () => {
    const current = listeners.get(key)
    if (current === undefined) return
    current.delete(listener)
    if (current.size === 0) listeners.delete(key)
  }
}

/// Drops the bookkeeping for a session that is gone.
export function forgetSessionFrames(key: string): void {
  versions.delete(key)
  listeners.delete(key)
}
