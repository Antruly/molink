import { useCallback, useEffect, useLayoutEffect, useRef, useState } from 'react'
import type { Frame } from '../lib/protocol'
import { sessionScreenBuffer } from '../lib/screen-buffer'

/**
 * Paints one session's composited back buffer.
 *
 * The buffer itself is built by the connection (screen-buffer.ts) for every
 * live tab, including the ones in the background, so this component only has to
 * blit it. Nothing here waits for a keyframe: switching tabs, entering
 * fullscreen or changing the display mode is a single drawImage and never
 * shows a cleared canvas.
 */
export type DisplayMode = 'fit' | 'actual' | 'fill'

export function TileCanvas({
  frame,
  mode = 'fit',
  sessionKey,
}: {
  frame: Frame | null
  mode?: DisplayMode
  sessionKey: string
}) {
  const canvasRef = useRef<HTMLCanvasElement>(null)
  const wrapRef = useRef<HTMLDivElement>(null)
  // Object-fit on a canvas is unreliable in WebKitGTK; size the element
  // explicitly so the painted image, the input mapping and the cursor overlay
  // all use the same rectangle.
  const [box, setBox] = useState({ width: 0, height: 0 })

  const back = sessionScreenBuffer(sessionKey)
  const screenW = back?.width ?? frame?.screenW ?? 0
  const screenH = back?.height ?? frame?.screenH ?? 0

  const blit = useCallback(() => {
    const canvas = canvasRef.current
    const source = sessionScreenBuffer(sessionKey)
    if (!canvas || !source || source.width === 0 || source.height === 0) return
    // Assigning width/height clears the surface, so only do it on a real size
    // change and draw right after.
    if (canvas.width !== source.width) canvas.width = source.width
    if (canvas.height !== source.height) canvas.height = source.height
    const context = canvas.getContext('2d', { alpha: false })
    if (!context) return
    context.imageSmoothingEnabled = false
    context.drawImage(source, 0, 0)
  }, [sessionKey])

  useEffect(() => {
    const element = wrapRef.current
    if (!element) return
    const update = () => {
      const width = element.clientWidth
      const height = element.clientHeight
      // A fullscreen transition can report a transient 0x0; collapsing the
      // canvas to it would leave a black surface until the next observation.
      if (width === 0 || height === 0) return
      setBox({ width, height })
    }
    update()
    const observer = new ResizeObserver(update)
    observer.observe(element)
    return () => observer.disconnect()
  }, [])

  // Draw before the browser paints, so a remounted canvas (tab switch, display
  // mode change) never shows an empty frame.
  useLayoutEffect(() => {
    blit()
  }, [blit, frame, mode, box.width, box.height])

  // A window/WM fullscreen transition can lose the canvas backing store
  // without producing a new frame (or before the resize observer runs). Blit
  // again on the next frame and after the allocation has settled.
  useEffect(() => {
    let frameRequest = 0
    let timers: number[] = []
    const repaint = () => {
      if (frameRequest !== 0) cancelAnimationFrame(frameRequest)
      frameRequest = requestAnimationFrame(() => {
        frameRequest = 0
        blit()
      })
      for (const timer of timers) window.clearTimeout(timer)
      // The native shell reallocates the WebView at 150 ms and 500 ms after a
      // fullscreen transition; repaint after both so a lost backing store is
      // restored even when the compositor redraws late.
      timers = [80, 250, 600].map((delay) => window.setTimeout(blit, delay))
    }
    window.addEventListener('resize', repaint)
    document.addEventListener('fullscreenchange', repaint)
    repaint()
    return () => {
      window.removeEventListener('resize', repaint)
      document.removeEventListener('fullscreenchange', repaint)
      if (frameRequest !== 0) cancelAnimationFrame(frameRequest)
      for (const timer of timers) window.clearTimeout(timer)
    }
  }, [blit])

  if (mode === 'actual') {
    return (
      <canvas
        ref={canvasRef}
        className="block max-w-none"
        style={screenW > 0 ? { width: screenW, height: screenH } : undefined}
        aria-label="Remote screen"
      />
    )
  }

  const fitScale =
    mode === 'fit' && screenW > 0 && box.width > 0 && box.height > 0
      ? Math.min(box.width / screenW, box.height / screenH)
      : 0
  const style =
    mode === 'fit'
      ? {
          width: screenW > 0 ? Math.round(screenW * fitScale) : 0,
          height: screenH > 0 ? Math.round(screenH * fitScale) : 0,
        }
      : undefined

  const debug =
    typeof window !== 'undefined' &&
    new URLSearchParams(window.location.search).has('debug')

  return (
    <div
      ref={wrapRef}
      className="relative flex h-full w-full items-center justify-center overflow-hidden"
    >
      <canvas
        ref={canvasRef}
        className={mode === 'fill' ? 'block h-full w-full' : 'block'}
        style={style}
        aria-label="Remote screen"
      />
      {debug ? (
        <div className="pointer-events-none absolute left-1 top-1 z-50 rounded bg-black/80 px-1 py-0.5 text-[10px] text-white">
          box {box.width}x{box.height} · canvas{' '}
          {`${Math.round((style?.width as number) ?? screenW)}x${Math.round((style?.height as number) ?? screenH)}`}{' '}
          · src {screenW}x{screenH} · {mode}
        </div>
      ) : null}
    </div>
  )
}
