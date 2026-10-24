import { useEffect, useRef, useState } from 'react'
import { FrameFlag, type Frame } from '../lib/protocol'
import { expandTile, tilePixelKind } from '../lib/tiles'

/**
 * Paints one frame. Tiles arrive as BGRA, RGB24 or RGB565 (raw or already
 * inflated); ImageData wants RGBA, so the channels are expanded while copying.
 * Any other codec is left to a later WebCodecs path, which is why the canvas
 * simply keeps the previous image.
 *
 * Tiles are painted into an off-screen back buffer and the completed buffer is
 * blitted in one drawImage. A delta can never leave the visible canvas half
 * updated, and a size change waits for the keyframe the core sends instead of
 * showing a cleared (black) canvas.
 */
export type DisplayMode = 'fit' | 'actual' | 'fill'

export function TileCanvas({
  frame,
  mode = 'fit',
}: {
  frame: Frame | null
  mode?: DisplayMode
}) {
  const canvasRef = useRef<HTMLCanvasElement>(null)
  const backRef = useRef<HTMLCanvasElement | null>(null)
  const wrapRef = useRef<HTMLDivElement>(null)
  // Object-fit on a canvas is unreliable in WebKitGTK; size the element
  // explicitly so the painted image, the input mapping and the cursor overlay
  // all use the same rectangle.
  const [box, setBox] = useState({ width: 0, height: 0 })

  useEffect(() => {
    const element = wrapRef.current
    if (!element) return
    const update = () =>
      setBox({ width: element.clientWidth, height: element.clientHeight })
    update()
    const observer = new ResizeObserver(update)
    observer.observe(element)
    return () => observer.disconnect()
  }, [])

  useEffect(() => {
    const canvas = canvasRef.current
    if (!canvas || !frame) return
    // Unknown codecs (PNG/H.264/AV1) wait for a WebCodecs path; keep showing
    // the previous image instead of blanking the back buffer.
    if (tilePixelKind(frame.codec) === null) return

    const keyframe = (frame.flags & FrameFlag.keyframe) !== 0
    let back = backRef.current
    if (!back || back.width !== frame.screenW || back.height !== frame.screenH) {
      // Deltas are relative to the previous frame. After a resolution change
      // they cannot be applied to the old buffer; the core sends a keyframe,
      // so keep the current image until it arrives.
      if (!keyframe) return
      back = document.createElement('canvas')
      back.width = frame.screenW
      back.height = frame.screenH
      backRef.current = back
    }

    const backContext = back.getContext('2d', { alpha: false })
    const context = canvas.getContext('2d', { alpha: false })
    if (!backContext || !context) return
    backContext.imageSmoothingEnabled = false
    context.imageSmoothingEnabled = false

    for (const tile of frame.tiles) {
      const rgba = expandTile(frame.codec, tile.data, tile.w, tile.h)
      if (rgba === null) continue
      backContext.putImageData(
        new ImageData(rgba, tile.w, tile.h),
        tile.x,
        tile.y,
      )
    }

    if (canvas.width !== frame.screenW) canvas.width = frame.screenW
    if (canvas.height !== frame.screenH) canvas.height = frame.screenH
    context.drawImage(back, 0, 0)
  }, [frame])

  if (mode === 'actual') {
    return (
      <canvas
        ref={canvasRef}
        className="block max-w-none"
        style={
          frame ? { width: frame.screenW, height: frame.screenH } : undefined
        }
        aria-label="Remote screen"
      />
    )
  }

  const fitScale =
    mode === 'fit' && frame && box.width > 0 && box.height > 0
      ? Math.min(box.width / frame.screenW, box.height / frame.screenH)
      : 0
  const style =
    mode === 'fit'
      ? {
          width: frame ? Math.round(frame.screenW * fitScale) : 0,
          height: frame ? Math.round(frame.screenH * fitScale) : 0,
        }
      : undefined

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
    </div>
  )
}
