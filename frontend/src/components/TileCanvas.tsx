import { useEffect, useRef } from 'react'
import { Codec, type Frame } from '../lib/protocol'

/**
 * Paints one frame. Raw tiles are BGRA; ImageData wants RGBA, so the red and
 * blue channels are swapped while copying. Any other codec is left to a later
 * WebCodecs path, which is why the canvas simply keeps the previous image.
 */
export function TileCanvas({ frame }: { frame: Frame | null }) {
  const canvasRef = useRef<HTMLCanvasElement>(null)

  useEffect(() => {
    const canvas = canvasRef.current
    if (!canvas || !frame) return
    if (canvas.width !== frame.screenW) canvas.width = frame.screenW
    if (canvas.height !== frame.screenH) canvas.height = frame.screenH

    const context = canvas.getContext('2d', { alpha: false })
    if (
      !context ||
      (frame.codec !== Codec.bgra && frame.codec !== Codec.deflateBgra)
    ) {
      return
    }

    for (const tile of frame.tiles) {
      const pixels = tile.w * tile.h
      if (tile.data.byteLength < pixels * 4) continue
      const image = context.createImageData(tile.w, tile.h)
      const dst = image.data
      const src = tile.data
      for (let i = 0; i < pixels * 4; i += 4) {
        dst[i] = src[i + 2]
        dst[i + 1] = src[i + 1]
        dst[i + 2] = src[i]
        dst[i + 3] = src[i + 3]
      }
      context.putImageData(image, tile.x, tile.y)
    }
  }, [frame])

  return (
    <canvas
      ref={canvasRef}
      className="h-full w-full object-contain"
      aria-label="Remote screen"
    />
  )
}
