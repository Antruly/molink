import { FrameFlag, type Frame } from './protocol'
import { expandTile, tilePixelKind } from './tiles'

/**
 * One composited back buffer per session, kept outside React.
 *
 * SessionView (and with it TileCanvas) only exists for the visible tab, but the
 * wire keeps delivering frames to every live tab in the background. Compositing
 * here means the buffer always holds the newest picture, so switching tabs,
 * entering fullscreen or changing the display mode only has to blit it instead
 * of waiting for a fresh keyframe (which used to show black for a while).
 */
const buffers = new Map<string, HTMLCanvasElement>()

/// Applies one frame to the session's picture. Deltas are painted over the
/// previous image; a resolution change waits for the core's keyframe.
export function applyFrameToSession(key: string, frame: Frame): void {
  if (key === '' || tilePixelKind(frame.codec) === null) return
  const keyframe = (frame.flags & FrameFlag.keyframe) !== 0
  let back = buffers.get(key)
  if (!back || back.width !== frame.screenW || back.height !== frame.screenH) {
    if (!keyframe) return
    back = document.createElement('canvas')
    back.width = frame.screenW
    back.height = frame.screenH
    buffers.set(key, back)
  }
  const context = back.getContext('2d', { alpha: false })
  if (!context) return
  context.imageSmoothingEnabled = false
  for (const tile of frame.tiles) {
    if (tile.w === 0 || tile.h === 0) continue
    const rgba = expandTile(frame.codec, tile.data, tile.w, tile.h)
    if (rgba === null) continue
    context.putImageData(new ImageData(rgba, tile.w, tile.h), tile.x, tile.y)
  }
}

/**
 * Applies one whole-screen stream frame (JPEG or H.264) to the session
 * picture. It arrives already decoded -- an ImageBitmap or a VideoFrame -- and
 * is blitted instead of being expanded pixel by pixel.
 */
export function applyStreamFrameToSession(
  key: string,
  frame: Frame,
  bitmaps: (ImageBitmap | VideoFrame)[],
): void {
  if (key === '') return
  const keyframe = (frame.flags & FrameFlag.keyframe) !== 0
  let back = buffers.get(key)
  if (!back || back.width !== frame.screenW || back.height !== frame.screenH) {
    if (!keyframe) return
    back = document.createElement('canvas')
    back.width = frame.screenW
    back.height = frame.screenH
    buffers.set(key, back)
  }
  const context = back.getContext('2d', { alpha: false })
  if (!context) return
  context.imageSmoothingEnabled = true
  for (let i = 0; i < frame.tiles.length && i < bitmaps.length; i += 1) {
    const tile = frame.tiles[i]!
    if (tile.w === 0 || tile.h === 0) continue
    context.drawImage(bitmaps[i]!, tile.x, tile.y, tile.w, tile.h)
  }
}

export function sessionScreenBuffer(key: string): HTMLCanvasElement | null {
  return buffers.get(key) ?? null
}

export function forgetSessionScreenBuffer(key: string): void {
  buffers.delete(key)
}
