import { useMemo } from 'react'
import type { CursorPosition, CursorShape, Welcome } from '../lib/client'

interface Props {
  shape: CursorShape | null
  position: CursorPosition | null
  welcome: Welcome | null
  viewWidth: number
  viewHeight: number
}

/// Draws the remote pointer. X11 screen grabs do not include the cursor, so
/// the core reports its bitmap and position separately. While the user has
/// control enabled this is hidden: their own pointer already shows where the
/// remote pointer is.
export function RemoteCursor({
  shape,
  position,
  welcome,
  viewWidth,
  viewHeight,
}: Props) {
  const src = useMemo(() => {
    if (!shape) return null
    const canvas = document.createElement('canvas')
    canvas.width = shape.w
    canvas.height = shape.h
    const context = canvas.getContext('2d')
    if (!context) return null
    const image = context.createImageData(shape.w, shape.h)
    image.data.set(shape.rgba)
    context.putImageData(image, 0, 0)
    return canvas.toDataURL()
  }, [shape])

  if (
    !src ||
    !shape ||
    !position ||
    !position.visible ||
    !welcome ||
    viewWidth <= 0 ||
    viewHeight <= 0
  ) {
    return null
  }

  const screenW = welcome.tiles.screen_w
  const screenH = welcome.tiles.screen_h
  if (screenW <= 0 || screenH <= 0) return null

  const scale = Math.min(viewWidth / screenW, viewHeight / screenH)
  const drawnWidth = screenW * scale
  const drawnHeight = screenH * scale
  const offsetX = (viewWidth - drawnWidth) / 2
  const offsetY = (viewHeight - drawnHeight) / 2

  const left = offsetX + (position.x - shape.hotspotX) * scale
  const top = offsetY + (position.y - shape.hotspotY) * scale

  return (
    <img
      data-remote-cursor
      src={src}
      alt=""
      draggable={false}
      className="pointer-events-none absolute z-10 select-none"
      style={{
        left,
        top,
        width: shape.w * scale,
        height: shape.h * scale,
        imageRendering: 'pixelated',
      }}
    />
  )
}
