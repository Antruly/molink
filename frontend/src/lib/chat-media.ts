/// Thumbnail generation for chat attachments.
///
/// Both entry points are best effort: a decoder that refuses a format, a
/// zero-length video or an enormous canvas returns null and the message falls
/// back to a type icon. Nothing here throws into React's render path.

import { attachmentKind, type AttachmentKind } from './chat'

export interface ChatThumbnail {
  /// `data:image/webp;base64,...` (or a JPEG fallback).
  dataUrl: string
  /// Size of the generated bitmap, for the bubble's aspect ratio.
  w: number
  h: number
  /// Video duration in seconds; 0 for images.
  dur: number
}

/// The issue's budget for the descriptor-sized preview. The image is uploaded
/// as a separate small file instead of being inlined in the chat text (the
/// chat hub caps that at 4096 bytes), but keeping the preview under 64 KiB
/// keeps that upload instantaneous.
export const MAX_THUMB_CHARS = 64 * 1024
export const MAX_THUMB_EDGE = 320

const VIDEO_LOAD_TIMEOUT_MS = 7000
const VIDEO_SEEK_TIMEOUT_MS = 4000

function scaledSize(width: number, height: number, edge: number): {
  w: number
  h: number
} {
  if (width <= 0 || height <= 0) return { w: Math.max(1, edge), h: Math.max(1, edge) }
  const largest = Math.max(width, height)
  if (largest <= edge) {
    return { w: Math.max(1, Math.round(width)), h: Math.max(1, Math.round(height)) }
  }
  const ratio = edge / largest
  return {
    w: Math.max(1, Math.round(width * ratio)),
    h: Math.max(1, Math.round(height * ratio)),
  }
}

function drawToCanvas(
  source: CanvasImageSource,
  width: number,
  height: number,
  edge: number,
): { canvas: HTMLCanvasElement; w: number; h: number } | null {
  const size = scaledSize(width, height, edge)
  const canvas = document.createElement('canvas')
  canvas.width = size.w
  canvas.height = size.h
  const context = canvas.getContext('2d')
  if (!context) return null
  context.drawImage(source, 0, 0, size.w, size.h)
  return { canvas, ...size }
}

/// Try WebP at the documented quality, then smaller edges/qualities, and only
/// then JPEG. Returns null when even the smallest candidate misses the budget.
function encodeCanvas(
  source: CanvasImageSource,
  width: number,
  height: number,
  duration: number,
): ChatThumbnail | null {
  const edges = [MAX_THUMB_EDGE, 288, 256, 224, 192, 160]
  const qualities = [0.8, 0.72, 0.64, 0.56, 0.48, 0.4]
  for (const edge of edges) {
    const drawn = drawToCanvas(source, width, height, edge)
    if (!drawn) return null
    for (const quality of qualities) {
      const webp = drawn.canvas.toDataURL('image/webp', quality)
      if (webp.length <= MAX_THUMB_CHARS) {
        return { dataUrl: webp, w: drawn.w, h: drawn.h, dur: duration }
      }
    }
  }
  // JPEG is larger for flat UI but still a valid last resort.
  const drawn = drawToCanvas(source, width, height, 192)
  if (drawn) {
    for (const quality of [0.7, 0.55, 0.4]) {
      const jpeg = drawn.canvas.toDataURL('image/jpeg', quality)
      if (jpeg.length <= MAX_THUMB_CHARS) {
        return { dataUrl: jpeg, w: drawn.w, h: drawn.h, dur: duration }
      }
    }
  }
  return null
}

function loadImageElement(file: Blob): Promise<HTMLImageElement> {
  return new Promise((resolve, reject) => {
    const url = URL.createObjectURL(file)
    const image = new Image()
    image.onload = () => {
      URL.revokeObjectURL(url)
      resolve(image)
    }
    image.onerror = () => {
      URL.revokeObjectURL(url)
      reject(new Error('image_decode_failed'))
    }
    image.src = url
  })
}

/// Scale an image to at most 320 px on its longest edge and encode WebP q0.8.
export async function imageThumbnail(file: Blob): Promise<ChatThumbnail | null> {
  try {
    let source: CanvasImageSource
    let width: number
    let height: number
    if (typeof createImageBitmap === 'function') {
      try {
        const bitmap = await createImageBitmap(file)
        source = bitmap
        width = bitmap.width
        height = bitmap.height
        try {
          return encodeCanvas(source, width, height, 0)
        } finally {
          bitmap.close()
        }
      } catch {
        // SVG and a few exotic formats are not bitmap-decodable.
      }
    }
    const image = await loadImageElement(file)
    source = image
    width = image.naturalWidth || image.width
    height = image.naturalHeight || image.height
    return encodeCanvas(source, width, height, 0)
  } catch {
    return null
  }
}

function waitFor(
  target: HTMLVideoElement,
  event: string,
  timeoutMs: number,
): Promise<void> {
  return new Promise((resolve, reject) => {
    let timer = 0
    const cleanup = () => {
      window.clearTimeout(timer)
      target.removeEventListener(event, onEvent)
      target.removeEventListener('error', onError)
    }
    const onEvent = () => {
      cleanup()
      resolve()
    }
    const onError = () => {
      cleanup()
      reject(new Error('video_decode_failed'))
    }
    timer = window.setTimeout(() => {
      cleanup()
      reject(new Error('video_timeout'))
    }, timeoutMs)
    target.addEventListener(event, onEvent)
    target.addEventListener('error', onError)
  })
}

/// Grab a frame near the first second (or the middle of a very short clip).
/// A browser that cannot decode the container returns null; the bubble then
/// shows the video icon plus the duration when the element reported one.
export async function videoThumbnail(file: Blob): Promise<ChatThumbnail | null> {
  const url = URL.createObjectURL(file)
  const video = document.createElement('video')
  video.muted = true
  video.playsInline = true
  video.preload = 'auto'
  video.style.position = 'fixed'
  video.style.left = '-9999px'
  video.style.top = '0'
  video.style.width = '2px'
  video.style.height = '2px'
  video.src = url
  document.body.appendChild(video)
  try {
    video.load()
    if (video.readyState < 2) {
      await waitFor(video, 'loadeddata', VIDEO_LOAD_TIMEOUT_MS)
    }
    const duration = Number.isFinite(video.duration) && video.duration > 0
      ? video.duration
      : 0
    if (duration > 0) {
      const target = Math.min(
        Math.max(0, duration > 2 ? 1 : duration / 2),
        Math.max(0, duration - 0.05),
      )
      if (Math.abs(video.currentTime - target) > 0.01) {
        const seeked = waitFor(video, 'seeked', VIDEO_SEEK_TIMEOUT_MS)
        video.currentTime = target
        await seeked
      }
    }
    const width = video.videoWidth
    const height = video.videoHeight
    if (width <= 0 || height <= 0) return null
    // A frame that cannot be drawn still leaves the duration, which the file
    // card shows beside the type icon.
    return encodeCanvas(video, width, height, duration) ?? {
      dataUrl: '',
      w: 0,
      h: 0,
      dur: duration,
    }
  } catch {
    return null
  } finally {
    video.removeAttribute('src')
    video.load()
    video.remove()
    URL.revokeObjectURL(url)
  }
}

/// Generate the descriptor's preview fields. Never throws.
export async function attachmentThumbnail(
  file: Blob,
  name: string,
  kind: AttachmentKind = attachmentKind(name, file.type),
): Promise<ChatThumbnail | null> {
  try {
    if (kind === 'image') return await imageThumbnail(file)
    if (kind === 'video') return await videoThumbnail(file)
  } catch {
    // Covered by the callers' fallback; this is belt and braces.
  }
  return null
}

/// Decode a `data:` URL into bytes for the thumbnail upload.
export function dataUrlToBlob(dataUrl: string): Blob {
  const comma = dataUrl.indexOf(',')
  if (comma < 0) return new Blob([])
  const header = dataUrl.slice(0, comma)
  const body = dataUrl.slice(comma + 1)
  const mime = /^data:([^;,]+)/.exec(header)?.[1] ?? 'application/octet-stream'
  if (!/;base64/i.test(header)) {
    try {
      return new Blob([decodeURIComponent(body)], { type: mime })
    } catch {
      return new Blob([body], { type: mime })
    }
  }
  try {
    const binary = atob(body)
    const bytes = new Uint8Array(binary.length)
    for (let index = 0; index < binary.length; index += 1) {
      bytes[index] = binary.charCodeAt(index)
    }
    return new Blob([bytes], { type: mime })
  } catch {
    return new Blob([])
  }
}
