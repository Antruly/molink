/// WebTransport transport for the page (Phase 5).
///
/// The host serves WebTransport on its own h3 port with a short-lived
/// certificate, because a browser only accepts a self-signed certificate
/// through `serverCertificateHashes` when it is valid for at most fourteen
/// days. The fingerprint the host advertises in `welcome` is exactly that
/// hash, so no CA and no manual trust are needed.
///
/// Two paths, mirroring the host:
///   * one bidirectional stream carries bare MOLK packets -- control, audio
///     and file, in the same framing the QUIC data stream uses;
///   * datagrams carry video, fragmented with an eight-byte header (frame id,
///     index, count) and reassembled here. A frame that loses a fragment is
///     dropped; the keyframe/repair loop covers the loss.

export interface WebTransportInfo {
  port: number
  /** SHA-256 of the certificate, colon-separated hex. */
  sha256: string
}

export interface WebTransportHandlers {
  onText: (text: string) => void
  onBinary: (data: Uint8Array) => void
  onClose: () => void
  onError?: (message: string) => void
}

export function webTransportSupported(): boolean {
  return typeof WebTransport !== 'undefined'
}

/// "ab:cd:ef:..." -> bytes, or null when the string is not a SHA-256 hash.
export function fingerprintBytes(hex: string): Uint8Array | null {
  const parts = hex.split(':')
  if (parts.length !== 32) return null
  const bytes = new Uint8Array(parts.length)
  for (let i = 0; i < parts.length; i += 1) {
    const value = Number.parseInt(parts[i]!, 16)
    if (!Number.isFinite(value) || value < 0 || value > 255) return null
    bytes[i] = value
  }
  return bytes
}

const kPacketHeader = 32
const kFragmentHeader = 8
const kDatagramPayload = 1100
const kMaxFrameBytes = 256 * 1024
/// First byte of a peer-opened stream that marks it as the file path.
const kFileStreamTag = 0x02

/// One video frame reassembled from its datagrams.
class DatagramAssembler {
  private frameId = 0
  private count = 0
  private parts = new Map<number, Uint8Array>()
  private bytes = 0

  push(data: Uint8Array): Uint8Array | null {
    if (data.length <= kFragmentHeader) return null
    const view = new DataView(data.buffer, data.byteOffset, data.byteLength)
    const frameId = view.getUint32(0, true)
    const index = view.getUint16(4, true)
    const count = view.getUint16(6, true)
    if (count === 0 || index >= count) return null
    if (frameId !== this.frameId) {
      // A newer frame supersedes whatever is still incomplete.
      this.frameId = frameId
      this.count = count
      this.parts = new Map()
      this.bytes = 0
    }
    if (this.count !== count || this.parts.has(index)) return null
    const payload = data.subarray(kFragmentHeader)
    if (this.bytes + payload.length > kMaxFrameBytes) return null
    this.bytes += payload.length
    this.parts.set(index, payload.slice())
    if (this.parts.size !== this.count) return null

    const out = new Uint8Array(this.bytes)
    let offset = 0
    for (const [key, part] of [...this.parts.entries()].sort((a, b) => a[0] - b[0])) {
      void key
      out.set(part, offset)
      offset += part.length
    }
    this.parts = new Map()
    this.bytes = 0
    return out
  }
}

export class WtTransport {
  private constructor(
    private readonly transport: WebTransport,
    private readonly writer: WritableStreamDefaultWriter<Uint8Array>,
    private readonly handlers: WebTransportHandlers,
  ) {}

  private fileWriter: WritableStreamDefaultWriter<Uint8Array> | null = null
  private assembler = new DatagramAssembler()
  private pending: Uint8Array[] = []
  private pendingBytes = 0
  private closed = false

  static async connect(
    url: string,
    certHash: Uint8Array,
    handlers: WebTransportHandlers,
  ): Promise<WtTransport | null> {
    if (typeof WebTransport === 'undefined') return null
    let transport: WebTransport | null = null
    try {
      transport = new WebTransport(url, {
        serverCertificateHashes: [
          {
            algorithm: 'sha-256',
            value: certHash.buffer.slice(
              certHash.byteOffset,
              certHash.byteOffset + certHash.byteLength,
            ) as ArrayBuffer,
          },
        ],
      })
      await transport.ready
    } catch {
      transport?.close()
      return null
    }
    if (transport === null) return null

    let writer: WritableStreamDefaultWriter<Uint8Array>
    try {
      const stream = await transport.createBidirectionalStream()
      writer = stream.writable.getWriter()
      const self = new WtTransport(transport, writer, handlers)
      void self.readStream(stream.readable)
      void self.readDatagrams()
      transport.closed.then(
        () => {
          self.closed = true
          handlers.onClose()
        },
        () => {
          self.closed = true
          handlers.onClose()
        },
      )
      return self
    } catch {
      transport.close()
      return null
    }
  }

  get isOpen(): boolean {
    return !this.closed
  }

  /// Opens the dedicated file stream: the first byte tags it, then the same
  /// MOLK framing as the control stream. A transfer then never sits in front
  /// of control messages.
  async openFileStream(): Promise<boolean> {
    if (this.closed) return false
    try {
      const stream = await this.transport.createBidirectionalStream()
      const writer = stream.writable.getWriter()
      await writer.write(new Uint8Array([kFileStreamTag]))
      this.fileWriter = writer
      return true
    } catch {
      return false
    }
  }

  /// Sends one whole MOLK packet on the file stream, false when it is not up.
  sendFile(data: Uint8Array): boolean {
    const writer = this.fileWriter
    if (this.closed || writer === null) return false
    writer.write(data).catch(() => this.fail('file stream write failed'))
    return true
  }

  /// Sends one whole MOLK packet on the reliable stream.
  sendBinary(data: Uint8Array): boolean {
    if (this.closed) return false
    this.writer.write(data).catch(() => this.fail('stream write failed'))
    return true
  }

  /// Text travels as a MOLK control packet, like the QUIC data stream: the
  /// host's stream parser expects MOLK framing, not bare JSON.
  sendText(text: string): boolean {
    return this.sendBinary(molkControlPacket(text))
  }

  /// Video goes out as fragmented datagrams; anything too large for that
  /// returns false so the caller can fall back to the stream.
  sendVideoDatagram(data: Uint8Array, frameId: number): boolean {
    if (this.closed || typeof this.transport.datagrams?.writable === 'undefined') {
      return false
    }
    if (data.length === 0 || data.length > kMaxFrameBytes) return false
    const chunk = kDatagramPayload - kFragmentHeader
    const count = Math.ceil(data.length / chunk)
    if (count > 0xffff) return false
    void (async () => {
      try {
        const writer = this.transport.datagrams.writable.getWriter()
        for (let index = 0; index < count; index += 1) {
          const start = index * chunk
          const slice = data.subarray(start, Math.min(start + chunk, data.length))
          const packet = new Uint8Array(kFragmentHeader + slice.length)
          const view = new DataView(packet.buffer)
          view.setUint32(0, frameId, true)
          view.setUint16(4, index, true)
          view.setUint16(6, count, true)
          packet.set(slice, kFragmentHeader)
          await writer.write(packet)
        }
        writer.releaseLock()
      } catch {
        this.fail('datagram write failed')
      }
    })()
    return true
  }

  close(): void {
    if (this.closed) return
    this.closed = true
    try {
      this.transport.close()
    } catch {
      // already closed
    }
  }

  private fail(message: string): void {
    if (this.closed) return
    this.handlers.onError?.(message)
    this.close()
  }

  private async readDatagrams(): Promise<void> {
    const readable = this.transport.datagrams?.readable
    if (readable === undefined) return
    const reader = readable.getReader()
    try {
      for (;;) {
        const { value, done } = await reader.read()
        if (done) return
        const frame = this.assembler.push(value)
        if (frame !== null) this.handlers.onBinary(frame)
      }
    } catch {
      // The session ended; closed handles the notification.
    }
  }

  private async readStream(readable: ReadableStream<Uint8Array>): Promise<void> {
    const reader = readable.getReader()
    try {
      for (;;) {
        const { value, done } = await reader.read()
        if (done) return
        this.feed(value)
      }
    } catch {
      // The session ended; closed handles the notification.
    }
  }

  /// Reassembles MOLK packets from the byte stream, the same way the host's
  /// stream_transport does: magic, then the payload length from the header.
  private feed(data: Uint8Array): void {
    this.pending.push(data)
    this.pendingBytes += data.length
    let all = new Uint8Array(this.pendingBytes)
    let offset = 0
    for (const part of this.pending) {
      all.set(part, offset)
      offset += part.length
    }
    let consumed = 0
    while (all.length - consumed >= kPacketHeader) {
      if (
        all[consumed] !== 0x4d || all[consumed + 1] !== 0x4f ||
        all[consumed + 2] !== 0x4c || all[consumed + 3] !== 0x4b
      ) {
        this.fail('bad packet magic on the WebTransport stream')
        return
      }
      const view = new DataView(all.buffer, all.byteOffset + consumed)
      const payload = view.getUint32(24, true)
      const total = kPacketHeader + payload
      if (all.length - consumed < total) break
      const type = all[consumed + 5]!
      if (type === 0x7f) {
        const body = all.subarray(consumed + kPacketHeader, consumed + total)
        this.handlers.onText(new TextDecoder().decode(body))
      } else {
        this.handlers.onBinary(all.slice(consumed, consumed + total))
      }
      consumed += total
    }
    if (consumed > 0) {
      all = all.slice(consumed)
      this.pendingBytes = all.length
      this.pending = all.length > 0 ? [all] : []
    }
  }
}

/// Wraps JSON in a MOLK control packet, which is what the host's stream
/// parser delivers to its text handler.
function molkControlPacket(text: string): Uint8Array {
  const body = new TextEncoder().encode(text)
  const packet = new Uint8Array(kPacketHeader + body.length)
  packet.set([0x4d, 0x4f, 0x4c, 0x4b], 0)  // "MOLK"
  packet[4] = 1                            // protocol version
  packet[5] = 0x7f                         // packet_type::control
  new DataView(packet.buffer).setUint32(24, body.length, true)
  packet.set(body, kPacketHeader)
  return packet
}
