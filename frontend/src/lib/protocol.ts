/**
 * TypeScript mirror of backend/src/protocol/frame_packet.h.
 *
 * The byte layout here must stay identical to the C++ header; it is the
 * contract between the core and every client. All integers are little-endian.
 */

export const PACKET_HEADER_BYTES = 32
export const FRAME_HEADER_BYTES = 16
export const TILE_ENTRY_BYTES = 16
export const PROTOCOL_VERSION = 1
export const MAX_PAYLOAD_BYTES = 16 * 1024 * 1024

export const PacketType = {
  hello: 0x01,
  frame: 0x02,
  ack: 0x03,
  input: 0x10,
  clipboard: 0x11,
  control: 0x7f,
  ping: 0x80,
  pong: 0x81,
} as const

export const Codec = {
  bgra: 1,
  png: 2,
  h264: 3,
  av1: 4,
} as const

export const FrameFlag = {
  keyframe: 1 << 0,
  partial: 1 << 1,
} as const

export const TileFlag = {
  changed: 1 << 0,
  keyframe: 1 << 1,
} as const

export class ProtocolError extends Error {}

export interface Packet {
  version: number
  type: number
  flags: number
  session: number
  seq: number
  tsUs: number
  payload: Uint8Array
}

export interface Tile {
  x: number
  y: number
  w: number
  h: number
  flags: number
  data: Uint8Array
}

export interface Frame {
  frameSeq: number
  tileCount: number
  codec: number
  flags: number
  screenW: number
  screenH: number
  tileW: number
  tileH: number
  tiles: Tile[]
}

function viewOf(bytes: Uint8Array): DataView {
  return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
}

export function decodePacket(input: ArrayBuffer | Uint8Array): Packet {
  const bytes = input instanceof Uint8Array ? input : new Uint8Array(input)
  if (bytes.byteLength < PACKET_HEADER_BYTES) {
    throw new ProtocolError('truncated packet header')
  }
  if (
    bytes[0] !== 0x4d ||
    bytes[1] !== 0x4f ||
    bytes[2] !== 0x4c ||
    bytes[3] !== 0x4b
  ) {
    throw new ProtocolError('bad magic (expected MOLK)')
  }
  const view = viewOf(bytes)
  const version = view.getUint8(4)
  if (version !== PROTOCOL_VERSION) {
    throw new ProtocolError(`unsupported protocol version ${version}`)
  }
  const payloadLen = view.getUint32(24, true)
  if (payloadLen > MAX_PAYLOAD_BYTES) {
    throw new ProtocolError('payload length exceeds limit')
  }
  if (payloadLen !== bytes.byteLength - PACKET_HEADER_BYTES) {
    throw new ProtocolError('payload length mismatch')
  }
  return {
    version,
    type: view.getUint8(5),
    flags: view.getUint16(6, true),
    session: view.getUint32(8, true),
    seq: view.getUint32(12, true),
    tsUs: Number(view.getBigUint64(16, true)),
    payload: bytes.subarray(PACKET_HEADER_BYTES),
  }
}

export function parseFramePayload(payload: Uint8Array): Frame {
  if (payload.byteLength < FRAME_HEADER_BYTES) {
    throw new ProtocolError('truncated frame header')
  }
  const view = viewOf(payload)
  const frameSeq = view.getUint32(0, true)
  const tileCount = view.getUint16(4, true)
  const codec = view.getUint8(6)
  const flags = view.getUint8(7)
  const screenW = view.getUint16(8, true)
  const screenH = view.getUint16(10, true)
  const tileW = view.getUint16(12, true)
  const tileH = view.getUint16(14, true)

  const tableEnd = FRAME_HEADER_BYTES + tileCount * TILE_ENTRY_BYTES
  if (tableEnd > payload.byteLength) {
    throw new ProtocolError('truncated tile table')
  }

  const tiles: Tile[] = []
  let offset = tableEnd
  for (let i = 0; i < tileCount; i += 1) {
    const entry = FRAME_HEADER_BYTES + i * TILE_ENTRY_BYTES
    const size = view.getUint32(entry + 12, true)
    if (size > payload.byteLength - offset) {
      throw new ProtocolError(`tile ${i} overruns frame payload`)
    }
    tiles.push({
      x: view.getUint16(entry + 0, true),
      y: view.getUint16(entry + 2, true),
      w: view.getUint16(entry + 4, true),
      h: view.getUint16(entry + 6, true),
      flags: view.getUint8(entry + 8),
      data: payload.subarray(offset, offset + size),
    })
    offset += size
  }
  if (offset !== payload.byteLength) {
    throw new ProtocolError('frame payload has trailing bytes')
  }

  return {
    frameSeq,
    tileCount,
    codec,
    flags,
    screenW,
    screenH,
    tileW,
    tileH,
    tiles,
  }
}

export function encodePacket(packet: Packet): Uint8Array {
  const out = new Uint8Array(PACKET_HEADER_BYTES + packet.payload.byteLength)
  out[0] = 0x4d
  out[1] = 0x4f
  out[2] = 0x4c
  out[3] = 0x4b
  const view = viewOf(out)
  view.setUint8(4, PROTOCOL_VERSION)
  view.setUint8(5, packet.type)
  view.setUint16(6, packet.flags, true)
  view.setUint32(8, packet.session, true)
  view.setUint32(12, packet.seq, true)
  view.setBigUint64(16, BigInt(packet.tsUs), true)
  view.setUint32(24, packet.payload.byteLength, true)
  view.setUint32(28, 0, true)
  out.set(packet.payload, PACKET_HEADER_BYTES)
  return out
}
