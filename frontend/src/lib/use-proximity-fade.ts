import {
  useCallback,
  useEffect,
  useRef,
  useState,
  type RefObject,
} from 'react'
import {
  DEFAULT_FADE_RADIUS,
  DEFAULT_RESTORE_DELAY_MS,
  isNearRect,
} from './proximity-fade'

export interface ProximityPointerEvent {
  clientX: number
  clientY: number
  /// Touch input has no hover; overlays stay readable on touch screens.
  pointerType?: string
}

export interface ProximityFadeOptions {
  enabled?: boolean
  radius?: number
  restoreDelayMs?: number
}

type Targets = Record<string, RefObject<HTMLElement | null>>

function emptyState(targets: Targets): Record<string, boolean> {
  const state: Record<string, boolean> = {}
  for (const key of Object.keys(targets)) state[key] = false
  return state
}

/**
 * Fades each overlay in `targets` independently while the pointer is within
 * `radius` of its rectangle.
 *
 * Deliberately not a listener of its own: the caller forwards the session
 * surface's existing `pointermove` event, so one pointer event produces one
 * rAF-throttled hit-test pass for every overlay (not one listener per overlay,
 * and no second `mousemove`). Only `opacity` changes downstream.
 */
export function useProximityFade(
  targets: Targets,
  options: ProximityFadeOptions = {},
) {
  const {
    enabled = true,
    radius = DEFAULT_FADE_RADIUS,
    restoreDelayMs = DEFAULT_RESTORE_DELAY_MS,
  } = options

  const [faded, setFaded] = useState<Record<string, boolean>>(() =>
    emptyState(targets),
  )

  // Refs mirror per-render values so the rAF callback and the timers never
  // capture a stale render.
  const fadedRef = useRef(faded)
  const targetsRef = useRef(targets)
  const radiusRef = useRef(radius)
  const restoreDelayRef = useRef(restoreDelayMs)
  const enabledRef = useRef(enabled)

  // Keep the latest values for the rAF callback and the timers; writing these
  // in an effect (rather than during render) also satisfies the refs rule.
  useEffect(() => {
    targetsRef.current = targets
  }, [targets])
  useEffect(() => {
    radiusRef.current = radius
    restoreDelayRef.current = restoreDelayMs
    enabledRef.current = enabled
  }, [enabled, radius, restoreDelayMs])

  const pointer = useRef<{ x: number; y: number } | null>(null)
  const raf = useRef<number | null>(null)
  const restoreTimers = useRef<Record<string, number>>({})

  const commit = useCallback((next: Record<string, boolean>) => {
    fadedRef.current = next
    setFaded(next)
  }, [])

  const measure = useCallback((key: string): DOMRect | null => {
    const element = targetsRef.current[key]?.current
    if (!element) return null
    const rect = element.getBoundingClientRect()
    // A hidden overlay reports 0×0 at (0, 0); never fade something invisible.
    return rect.width > 0 || rect.height > 0 ? rect : null
  }, [])

  const scheduleRestore = useCallback((key: string) => {
    if (restoreTimers.current[key] !== undefined) return
    restoreTimers.current[key] = window.setTimeout(() => {
      delete restoreTimers.current[key]
      const position = pointer.current
      const rect = measure(key)
      const near =
        position !== null &&
        rect !== null &&
        isNearRect(position.x, position.y, rect, radiusRef.current)
      if (near) return
      if (!fadedRef.current[key]) return
      commit({ ...fadedRef.current, [key]: false })
    }, restoreDelayRef.current)
  }, [commit, measure])

  const clearRestore = useCallback((key: string) => {
    const timer = restoreTimers.current[key]
    if (timer === undefined) return
    window.clearTimeout(timer)
    delete restoreTimers.current[key]
  }, [])

  const evaluate = useCallback(() => {
    raf.current = null
    const position = pointer.current
    if (!position || !enabledRef.current) return
    const next = { ...fadedRef.current }
    let changed = false
    for (const key of Object.keys(targetsRef.current)) {
      const rect = measure(key)
      const near =
        rect !== null &&
        isNearRect(position.x, position.y, rect, radiusRef.current)
      if (near) {
        clearRestore(key)
        if (!next[key]) {
          next[key] = true
          changed = true
        }
      } else if (next[key]) {
        // Leave the overlay faded during the debounce; if the pointer comes
        // back the next pass clears the timer, so an edge does not flicker.
        scheduleRestore(key)
      }
    }
    if (changed) commit(next)
  }, [clearRestore, commit, measure, scheduleRestore])

  const evaluateRef = useRef(evaluate)
  useEffect(() => {
    evaluateRef.current = evaluate
  }, [evaluate])

  const onPointerMove = useCallback((event: ProximityPointerEvent) => {
    if (!enabledRef.current) return
    if (event.pointerType === 'touch') return
    pointer.current = { x: event.clientX, y: event.clientY }
    if (raf.current !== null) return
    raf.current = window.requestAnimationFrame(() => evaluateRef.current())
  }, [])

  const onPointerLeave = useCallback(() => {
    pointer.current = null
    if (raf.current !== null) {
      window.cancelAnimationFrame(raf.current)
      raf.current = null
    }
    for (const key of Object.keys(targetsRef.current)) {
      if (fadedRef.current[key]) scheduleRestore(key)
    }
  }, [scheduleRestore])

  // Turning the preference off restores every overlay immediately and drops
  // any measurement in flight.
  useEffect(() => {
    if (enabled) return
    pointer.current = null
    if (raf.current !== null) {
      window.cancelAnimationFrame(raf.current)
      raf.current = null
    }
    for (const key of Object.keys(restoreTimers.current)) {
      window.clearTimeout(restoreTimers.current[key])
    }
    restoreTimers.current = {}
    if (Object.values(fadedRef.current).some(Boolean)) {
      commit(emptyState(targetsRef.current))
    }
  }, [enabled, commit])

  useEffect(
    () => () => {
      if (raf.current !== null) window.cancelAnimationFrame(raf.current)
      for (const timer of Object.values(restoreTimers.current)) {
        window.clearTimeout(timer)
      }
      restoreTimers.current = {}
    },
    [],
  )

  return { faded, onPointerMove, onPointerLeave }
}
