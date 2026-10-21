import { useEffect, useRef } from 'react'
import { Codec, FrameFlag, type Frame } from '../lib/protocol'

/**
 * Paints one frame. Raw tiles are BGRA; ImageData wants RGBA, so the red and
 * blue channels are swapped while copying. Any other codec is left to a later
 * WebCodecs path, which is why the canvas simply keeps the previous image.
 *
 * Tiles are painted into an off-screen back buffer and the completed buffer is
 * blitted in one drawImage. A delta can never leave the visible canvas half
 * updated, and a size change waits for the keyframe the core sends instead of
 * showing a cleared (black) canvas.
 */
export function TileCanvas({ frame }: { frame: Frame | null }) {
  const canvasRef = useRef<HTMLCanvasElement>(null)
  const backRef = useRef<HTMLCanvasElement | null>(null)

  useEffect(() => {
    const canvas = canvasRef.current
    if (!canvas || !frame) return
    if (frame.codec !== Codec.bgra && frame.codec !== Codec.deflateBgra) return

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
      const pixels = tile.w * tile.h
      if (tile.data.byteLength < pixels * 4) continue
      const image = backContext.createImageData(tile.w, tile.h)
      const dst = image.data
      const src = tile.data
      for (let i = 0; i < pixels * 4; i += 4) {
        dst[i] = src[i + 2]
        dst[i + 1] = src[i + 1]
        dst[i + 2] = src[i]
        dst[i + 3] = src[i + 3]
      }
      backContext.putImageData(image, tile.x, tile.y)
    }

    if (canvas.width !== frame.screenW) canvas.width = frame.screenW
    if (canvas.height !== frame.screenH) canvas.height = frame.screenH
    context.drawImage(back, 0, 0)
  }, [frame])

  return (
    <canvas
      ref={canvasRef}
      className="h-full w-full object-contain"
      aria-label="Remote screen"
    />
  )
}
