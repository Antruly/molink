/// Decodes the host's low-delay H.264 stream with WebCodecs.
///
/// The host encodes a baseline H.264 stream on the GPU when not every viewer
/// can decode AV1. Frames arrive as Annex-B access units: the keyframe carries
/// SPS/PPS inline, later ones are P frames that only decode on top of it.
///
/// The decoder is created on the first keyframe, because the SPS is what says
/// which profile and level the stream is. When the decoder falls behind, this
/// class drops a delta instead of queueing an ever-growing backlog -- the
/// caller asks for a keyframe, which resynchronises the whole chain.

export function h264StreamSupported(): Promise<boolean> {
  if (typeof VideoDecoder === 'undefined') return Promise.resolve(false)
  // Baseline is what the host's NVENC session is configured for; asking about
  // it is enough to tell whether this browser can decode the stream at all.
  return VideoDecoder.isConfigSupported({ codec: 'avc1.42E01E' })
    .then((result) => result.supported === true)
    .catch(() => false)
}

/// Builds the WebCodecs codec string from the SPS in an Annex-B access unit:
/// `avc1.PPCCLL` is profile_idc, constraint flags and level_idc, all read from
/// the bitstream so the decoder is configured for what actually arrives.
export function h264CodecString(bytes: Uint8Array): string | null {
  const hex = (value: number) => value.toString(16).padStart(2, '0')
  for (let i = 0; i + 4 < bytes.length; i += 1) {
    let header = -1
    if (bytes[i] === 0 && bytes[i + 1] === 0 && bytes[i + 2] === 1) {
      header = i + 3
    } else if (
      bytes[i] === 0 && bytes[i + 1] === 0 && bytes[i + 2] === 0 &&
      bytes[i + 3] === 1
    ) {
      header = i + 4
    }
    if (header < 0 || header + 4 > bytes.length) continue
    if ((bytes[header]! & 0x1f) !== 7) continue  // 7 = SPS
    return `avc1.${hex(bytes[header + 1]!)}${hex(bytes[header + 2]!)}${hex(
      bytes[header + 3]!)}`
  }
  return null
}

/// Deltas allowed in the decoder queue before frames are dropped. WebCodecs
/// decodes asynchronously; letting this grow means the picture runs further and
/// further behind the capture instead of skipping.
const kMaxQueuedDeltas = 2

export class H264StreamDecoder {
  private decoder: VideoDecoder | null = null
  private pending: Array<(frame: VideoFrame | null) => void> = []
  private broken = false
  private timestamp = 0
  private dropped = 0

  /// True when the decoder was created and has not failed.
  get active(): boolean {
    return this.decoder !== null && !this.broken
  }

  /// Frames dropped because the decoder was behind (diagnostics and for the
  /// caller's decision to ask for a keyframe).
  get droppedFrames(): number {
    return this.dropped
  }

  /// Decodes one access unit. Resolves with the picture it produced, or null
  /// when the frame was dropped, the stream did not start at a keyframe, or
  /// the decoder failed.
  async decode(bytes: Uint8Array, keyframe: boolean): Promise<VideoFrame | null> {
    if (this.broken) return null
    if (this.decoder === null) {
      if (!keyframe) return null   // a delta has nothing to reference yet
      const codec = h264CodecString(bytes)
      if (codec === null) return null
      const decoder = new VideoDecoder({
        output: (frame) => {
          const resolve = this.pending.shift()
          if (resolve) resolve(frame)
          else frame.close()
        },
        error: () => {
          this.broken = true
          this.settleAll(null)
        },
      })
      try {
        // `avc.format` is the AVC extension of VideoDecoderConfig: Annex-B
        // input with the parameter sets inline (Chrome supports it; the DOM
        // typings in this project predate the extension).
        decoder.configure({
          codec,
          optimizeForLatency: true,
          avc: { format: 'annexb' },
        } as VideoDecoderConfig)
      } catch {
        this.broken = true
        return null
      }
      this.decoder = decoder
    }
    const decoder = this.decoder
    if (decoder.state !== 'configured') return null
    if (!keyframe && decoder.decodeQueueSize >= kMaxQueuedDeltas) {
      // Behind: dropping one delta costs a resync, queueing it costs latency
      // that never comes back.
      this.dropped += 1
      return null
    }
    const picture = new Promise<VideoFrame | null>((resolve) => {
      this.pending.push(resolve)
    })
    try {
      decoder.decode(
        new EncodedVideoChunk({
          type: keyframe ? 'key' : 'delta',
          timestamp: this.timestamp,
          data: bytes,
        }),
      )
      this.timestamp += 1000
    } catch {
      this.pending.pop()
      this.broken = true
      this.settleAll(null)
      return null
    }
    return picture
  }

  /// Drops the decoder. A later keyframe builds a fresh one.
  close(): void {
    this.settleAll(null)
    if (this.decoder !== null) {
      try {
        if (this.decoder.state !== 'closed') this.decoder.close()
      } catch {
        // A decoder that already failed is closed by the error path.
      }
      this.decoder = null
    }
    this.broken = false
    this.dropped = 0
  }

  private settleAll(frame: VideoFrame | null): void {
    const waiting = this.pending
    this.pending = []
    for (const resolve of waiting) resolve(frame)
  }
}
