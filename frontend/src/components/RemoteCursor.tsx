import { useMemo } from 'react'
import type { CursorPosition, CursorShape, Welcome } from '../lib/client'
import type { DisplayMode } from './TileCanvas'

interface Props {
  shape: CursorShape | null
  position: CursorPosition | null
  welcome: Welcome | null
  viewWidth: number
  viewHeight: number
  screenWidth?: number
  screenHeight?: number
  mode?: DisplayMode
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
  screenWidth,
  screenHeight,
  mode = 'fit',
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

  const screenW =
    screenWidth && screenWidth > 0 ? screenWidth : welcome.tiles.screen_w
  const screenH =
    screenHeight && screenHeight > 0 ? screenHeight : welcome.tiles.screen_h
  if (screenW <= 0 || screenH <= 0) return null

  if (mode === 'actual') {
    return (
      <img
        data-remote-cursor
        src={src}
        alt=""
        draggable={false}
        className="pointer-events-none absolute z-10 select-none"
        style={{
          left: position.x - shape.hotspotX,
          top: position.y - shape.hotspotY,
          width: shape.w,
          height: shape.h,
          imageRendering: 'pixelated',
        }}
      />
    )
  }

  const scale =
    mode === 'fill'
      ? 1
      : Math.min(viewWidth / screenW, viewHeight / screenH)
  const scaleX = mode === 'fill' ? viewWidth / screenW : scale
  const scaleY = mode === 'fill' ? viewHeight / screenH : scale
  const offsetX =
    mode === 'fill' ? 0 : (viewWidth - screenW * scaleX) / 2
  const offsetY =
    mode === 'fill' ? 0 : (viewHeight - screenH * scaleY) / 2

  const left = offsetX + (position.x - shape.hotspotX) * scaleX
  const top = offsetY + (position.y - shape.hotspotY) * scaleY

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
        width: shape.w * scaleX,
        height: shape.h * scaleY,
        imageRendering: 'pixelated',
      }}
    />
  )
}
