import { Codec, codecBytesPerPixel } from './protocol'

/**
 * How a tile's bytes map to pixels. The wire format is deliberately simple:
 * deflate wrappers are removed by the client before this runs, so only the
 * pixel layout matters here.
 */
export type TilePixelKind = 'bgra' | 'rgb24' | 'rgb565'

export function tilePixelKind(codec: number): TilePixelKind | null {
  switch (codec) {
    case Codec.bgra:
    case Codec.deflateBgra:
      return 'bgra'
    case Codec.rgb24:
    case Codec.deflateRgb24:
      return 'rgb24'
    case Codec.rgb565:
    case Codec.deflateRgb565:
      return 'rgb565'
    default:
      return null
  }
}

/**
 * Expand one tile to RGBA for `ImageData`. Returns null when the codec is not
 * pixel-based or the buffer is shorter than the tile claims.
 *
 * BGRA swaps red and blue; RGB24 drops the alpha channel; RGB565 is
 * little-endian with 5 bits red, 6 green and 5 blue, expanded by bit
 * replication so white stays white.
 */
/// Bit-replicated expansions for RGB565. Building them once turns the inner
/// loop into three table reads and one 32-bit store.
const EXPAND5 = new Uint8Array(32)
const EXPAND6 = new Uint8Array(64)
for (let i = 0; i < 32; i += 1) EXPAND5[i] = (i << 3) | (i >> 2)
for (let i = 0; i < 64; i += 1) EXPAND6[i] = (i << 2) | (i >> 4)

export function expandTile(
  codec: number,
  data: Uint8Array,
  width: number,
  height: number,
): Uint8ClampedArray<ArrayBuffer> | null {
  const kind = tilePixelKind(codec)
  const bytesPerPixel = codecBytesPerPixel(codec)
  if (kind === null || bytesPerPixel === 0) return null
  const pixels = width * height
  if (data.byteLength < pixels * bytesPerPixel) return null

  const rgba = new Uint8ClampedArray(pixels * 4)
  // One 32-bit store per pixel is much cheaper than four clamped byte stores.
  // RGBA bytes are written as a little-endian word: A<<24 | B<<16 | G<<8 | R.
  const out = new Uint32Array(rgba.buffer)

  if (kind === 'bgra') {
    if (data.byteOffset % 4 === 0) {
      // Inflated tile data is freshly allocated and therefore aligned; raw
      // BGRA can be a subarray, hence the byte fallback below.
      const src = new Uint32Array(
        data.buffer as ArrayBuffer,
        data.byteOffset,
        pixels,
      )
      for (let i = 0; i < pixels; i += 1) {
        const v = src[i]
        out[i] =
          (v & 0xff000000) |
          ((v & 0xff) << 16) |
          (v & 0x00ff00) |
          ((v >>> 16) & 0xff)
      }
    } else {
      for (let i = 0, j = 0; i < pixels; i += 1, j += 4) {
        out[i] =
          0xff000000 | (data[j + 2] << 16) | (data[j + 1] << 8) | data[j]
      }
    }
  } else if (kind === 'rgb24') {
    for (let i = 0, j = 0; i < pixels; i += 1, j += 3) {
      out[i] =
        0xff000000 | (data[j + 2] << 16) | (data[j + 1] << 8) | data[j]
    }
  } else if (data.byteOffset % 2 === 0) {
    const src = new Uint16Array(
      data.buffer as ArrayBuffer,
      data.byteOffset,
      pixels,
    )
    for (let i = 0; i < pixels; i += 1) {
      const v = src[i]
      out[i] =
        0xff000000 |
        (EXPAND5[v >> 11] << 16) |
        (EXPAND6[(v >> 5) & 0x3f] << 8) |
        EXPAND5[v & 0x1f]
    }
  } else {
    for (let i = 0, j = 0; i < pixels; i += 1, j += 2) {
      const v = data[j] | (data[j + 1] << 8)
      out[i] =
        0xff000000 |
        (EXPAND5[v >> 11] << 16) |
        (EXPAND6[(v >> 5) & 0x3f] << 8) |
        EXPAND5[v & 0x1f]
    }
  }
  return rgba
}
