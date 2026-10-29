/// Decodes the host's low-delay VP8/VP9 streams with WebCodecs.
///
/// Unlike H.264 there is no Annex-B/AVCC conversion and no parameter-set
/// description: libvpx hands over raw codec frames, and the decoder is
/// configured with just the codec string. The queue/drop policy mirrors the
/// H.264 decoder so a decoder that falls behind skips deltas instead of
/// growing a backlog.

const kMaxQueuedDeltas = 2

export type VpxCodec = 'vp8' | 'vp9'

function codecString(codec: VpxCodec): string {
  // VP9 profile 0, level 1.0, 8-bit: what libvpx emits for our stream.
  return codec === 'vp8' ? 'vp8' : 'vp09.00.10.08'
}

export function vpxStreamSupported(codec: VpxCodec): Promise<boolean> {
  if (typeof VideoDecoder === 'undefined') return Promise.resolve(false)
  return VideoDecoder.isConfigSupported({ codec: codecString(codec) })
    .then((result) => result.supported === true)
    .catch(() => false)
}

export class VpxStreamDecoder {
  private decoder: VideoDecoder | null = null
  private pending: Array<(frame: VideoFrame | null) => void> = []
  private timestamp = 0
  private broken = false
  private dropped = 0

  constructor(private readonly codec: VpxCodec) {}

  get active(): boolean {
    return this.decoder !== null && !this.broken
  }

  get droppedFrames(): number {
    return this.dropped
  }

  /// Decodes one frame. Resolves with the picture, or null when the frame was
  /// dropped or the decoder failed.
  async decode(bytes: Uint8Array, keyframe: boolean): Promise<VideoFrame | null> {
    if (this.broken) return null
    if (this.decoder === null) {
      if (!keyframe) return null
      const decoder = new VideoDecoder({
        output: (frame) => {
          const resolve = this.pending.shift()
          if (resolve) resolve(frame)
          else frame.close()
        },
        error: () => {
          // Drop this decoder; the next keyframe builds a fresh one.
          this.close()
        },
      })
      try {
        decoder.configure({
          codec: codecString(this.codec),
          optimizeForLatency: true,
        })
      } catch {
        this.close()
        return null
      }
      this.decoder = decoder
    }
    const decoder = this.decoder
    if (decoder.state !== 'configured') return null
    if (!keyframe && decoder.decodeQueueSize >= kMaxQueuedDeltas) {
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
      this.close()
      return null
    }
    return picture
  }

  close(): void {
    this.settleAll(null)
    if (this.decoder !== null) {
      try {
        if (this.decoder.state !== 'closed') this.decoder.close()
      } catch {
        // Already failed and closed by the error path.
      }
      this.decoder = null
    }
    this.broken = false
    this.dropped = 0
  }

  private settleAll(frame: VideoFrame | null): void {
    for (const resolve of this.pending) resolve(frame)
    this.pending = []
  }
}
