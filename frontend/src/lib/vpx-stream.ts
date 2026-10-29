/// Decodes the host's low-delay VP8/VP9/AV1 streams with WebCodecs.
///
/// Unlike H.264 there is no Annex-B/AVCC conversion and no parameter-set
/// description: the host hands over raw codec frames, and the decoder is
/// configured with just the codec string. The queue/drop policy mirrors the
/// H.264 decoder so a decoder that falls behind skips deltas instead of
/// growing a backlog.
///
/// `streamCodecDecodes` decides whether a codec is offered at all: accepting
/// the configuration is not the same as being able to decode it (WebKitGTK
/// before 2.54 fails every AV1 frame, WebKit bug 327481).

const kMaxQueuedDeltas = 2

export type VpxCodec = 'vp8' | 'vp9' | 'av1'

function codecString(codec: VpxCodec): string {
  if (codec === 'vp8') return 'vp8'
  // VP9 profile 0, level 1.0, 8-bit: what libvpx emits for our stream.
  if (codec === 'vp9') return 'vp09.00.10.08'
  // AV1 main profile, level 3.1: what NVENC/libaom emit for the AV1 stream.
  return 'av01.0.05M.08'
}

/// One 64x64 keyframe per stream codec. Decoding it proves the WebView can
/// really decode the codec, not just accept the configuration.
const kProbeFrames: Record<VpxCodec, string> = {
  // AV1 keyframe (SVT-AV1, main profile, 8-bit).
  av1: 'EgAKDQAAAAKv/41fIgwMDAgyugMUAK6AkLefLXkKwxKt+ZoU8L54EPWFu4brjb7NdtPkLA2SfLqSgJgRTaFOuk15ny180Aow8qOjYZFlkbUFHnCee1jD54FhhXA888nL+JEdGvBJDOy7SxNsouQMfr+xy1mvPVHJVpb8Ot0Sy0e05hciZFAxkbtom8fpjP0ioOD0mGYCvR/iRU0a3TYYnH33aaPIphSt1pP37RQdqGnkMcRkGnIEDWcvVpDDtbByvTluU7IFFucjoVvDDn5JOYsheg75TPId/m6XSUAJy9q4a///S4La4lYkMcYT7eYhFwaue8FOxSQw17ChUU/iMubbAe3hXi0ZdhX1HJpMv0q03WDQ5m4Wb4GD+ajexZ9oRlus6XkiVuF6jjMjju9o8xl1raJTdwzFfofedzWD9pOry4Xv92cBq2rv7dAwPIUnyxO+GQV+6B459GWjnKIgI/bWtMF2jgtVm9xP3aUnpNlAgDJ5lMfZ+f/qwkE88icJuU4sG1YhtOTV2texPIgxtVIgUXkTD4Bsw3rZPKwhMOmkSkMC7M6wA1acpRCy2ypMdshSiH9FM9WD34jfrhoOTwjQG53zX+s2x6SUYW9I',
  // VP9 keyframe (profile 0, 8-bit).
  vp9: 'gkmDQiAD8AP2CDgkHBhCAAIAb7F4rp+Zq9pZ1/P7ZNQ/H1EDPnzUav38aI69oSOAAAB+GHFK1f122Q5Y4adRTvpe/hlZ1qJQ/qIxu8HWW22WHP7ebWy/4TdzgeewZTfiI0OnDtTGq40kRgABsXwB1/SDMrDMyX+fiCBvvf5x6t3crmf2gMjXnKZcBfSZF71wC27rO4ocvlB1iA3TykEop1pUOA2IOEaOuzurxOFDt5YVjaZ+2M1VRd+RlTusiXP0bu3fACfl9x4U5NkSnpKE8jzIPy2g55NT4yl6+Gdr6ggSiwfuzpn9BTjqg3qcgXu4nQuKNeIiPt019usf3p7N8AuaAV0kncY3C++B//yCRubOD//9mxdx25rxfZkjPOiHB/dhBwVsKGKmzltko/1Q2Yajdpxs2cS8IG8JohzP1Ap2XJA5TfkglR/X/6a9fyGT60Ro74CWWnf2gkjC/vu+pOVxTSdIwH34d6ja2j8ZYXfIETioa8fInMw2SW5GezF9wBspxjPf/Qccgvo8b3R01HtQeTY3NO8smJnuFvg32Oe+tRbWWA/klMTJJ4lVRMMitS/BL/f9VIIVfvLNssO7KBLUp2rx5ixlsQIHSSwgd7Xvglp9qJsfnee3z/jJ/671uqnGH3Y3WsQz0kNr/HXvYC+Gnvsonma//1OXjhx9oB8lhOlePkFvApr30+ELh6MG4Rn/7mP/9aE4Ph50aTwYEl3x2HrYQrtoWXt4bLtiXABXQnuU+39oVV5V3e5YT2Mg7elRr1MI580xPljTbR39zv4cWYz8hFKH4zYT4sFnrC4OQl0aSxKV8Y5J/ntd/u9+sd6vDSlfIKxoZoWPPbrCofXA7oG48yh2k+h+jIt9lbDKeAiQaENQ16/1V4/wiCjtkOVqm7Tsg9oAAA==',
  // VP8 keyframe.
  vp8: 'MBwAnQEqQABAAABHCIWFiIWEiAICAzx+mPR+A/gT+p/+g6FHafsD+rvsA/gD2n8c/wH8AfAF+I/h9/bvcD/AfyA7AH/k/wH+OfhBwAf0B/on9A9hH+k9JB6EvWO/wH+I+hF/T+oUzQH+A/gl9s3kB5I/0ByTHxP8Zv280UX4n/lBtoHYzeQL+k8YD/JfxV9wP45/IOID/Wvxi9ib8z/FDyJfyA+SD+Hfy38Sf6x/8voB6gH9EefTqsSRW7eofnQhqOuc07A6RRRbuHE1UUJE/K+H2hIskCpTxQhfPJxTv/1EztLBWjBqNzwGQP4cHpK7oZB0Mh1HdNsMYzUqJxZmcRye1vnGG7zbKTMpqiv9IcH1KKbyPEdBqS+kKR2VmXlkUM15aRAZuOpxwteWkQGb69B/quFuhzLYI8tO30ZS+cmB4qCm1+s78UYtWoku8FAzqJHBqWGt6jSHIVmNOXHxiN2ujdocYxuWZiobONLgxrtNiGsixQUsFCZ2W4+BRTYlWLWqHX//rMGDE5///o/nFvAZb8CJJ7Qa3cvsv/5CAZFcJb/Gn9grooJqgtssfTfq6Gca/+TyekrpDUzF2fW4DLN+O6o1gpfO+SCTU7H5n/ozSF0SW575MiXgEajJqizXV50wBbCdUwFGKSrOjQ74CUw3wGkA0TgIR//aH28KwDDA9yOYRBcjxKIc1PxIiVHMKi+3tcfWkyASutoZ0KYJAuaH3Bn/+1DGZjjiHLH6oh79i/+32z/xa+ARkni1MZ86EfL7JOb3y+FnxQxFQ43mCU5lJ9wDRH5bN9x8XnfnP+GhhQGlwr55IevnN9RlPa1+jTp9tXfDabVqOp/FUs9FYbWRVbA25jYtVtqmGPdbnN8MvIndd2e4JflXWrIn6mrnDKne0APYKQx4l3J7rITzRJ1yEaXQA//rcBjNf3yny8Y/kXnHoHCxYjOa8dbkEMtgdVfEl1eHHvFKR6jMcE4AnufMCsd0s+zfPRn2qq+cP+NxDN+U3tsanB1iL1hUJeqjraXzv9IqT1mPHdti0So4wvxuMyZZzQv5jK+2NeGGoz6T/jg9Fb5fAZ1Ljn+X3fnoXrZ+MSN0cpAsjpqGxgj0JWkLA7gpcURc7Mv6j4pjA4C90cJYBtf9ePu/ZdmET6azX+H+JAoKR98d5JiVbBkc6S7Fm8lDS3+66Mdfn80f/Hw0t/7Gx9/2At7KOPKhkEmXCx5cgRm3k4Fou/l/+BaRuEfbYzKVZJ18Hz1CP9g2z/mwp//cBHXpNlq9MlvwVf/ngAAA',
}

function base64ToBytes(text: string): Uint8Array {
  const binary = atob(text)
  const bytes = new Uint8Array(binary.length)
  for (let i = 0; i < binary.length; i += 1) bytes[i] = binary.charCodeAt(i)
  return bytes
}

/// True when this WebView decodes an actual frame of `codec`.
///
/// `isConfigSupported` is not enough: WebKitGTK before 2.54 accepts the AV1
/// configuration but pushes the AV1 caps straight into the decoder without an
/// `av1parse` in front, so every frame fails with "Decode error" (WebKit bug
/// 327481). The page then claims AV1, the host streams AV1, and the picture
/// freezes after the first second. Decode one keyframe before claiming a
/// codec, so a WebView that cannot really decode it never asks the host for
/// it.
export function streamCodecDecodes(codec: VpxCodec): Promise<boolean> {
  if (typeof VideoDecoder === 'undefined') return Promise.resolve(false)
  return VideoDecoder.isConfigSupported({ codec: codecString(codec) })
    .then((result) => {
      if (result.supported !== true) return false
      return new Promise<boolean>((resolve) => {
        let done = false
        let decoder: VideoDecoder | null = null
        let timer = 0
        const finish = (ok: boolean) => {
          if (done) return
          done = true
          window.clearTimeout(timer)
          if (decoder !== null) {
            try {
              decoder.close()
            } catch {
              // Already failed and closed by the error path.
            }
          }
          resolve(ok)
        }
        // A WebKit decoder emits a picture when the next chunk arrives, not
        // when the chunk it already has is decoded. A probe that ends on a
        // timeout therefore says nothing about VP8/VP9 — only an explicit
        // decoder error does. AV1's failure is the known WebKit bug 327481,
        // so there a timeout still means "cannot decode".
        const timeoutMeansBroken = codec === 'av1'
        timer = window.setTimeout(() => finish(timeoutMeansBroken ? false : true), 2000)
        try {
          decoder = new VideoDecoder({
            output: (frame) => {
              frame.close()
              finish(true)
            },
            error: () => finish(false),
          })
          decoder.configure({
            codec: codecString(codec),
            optimizeForLatency: true,
          })
          const frame = base64ToBytes(kProbeFrames[codec])
          // The same keyframe twice: the second chunk is what makes WebKit
          // emit the picture for the first.
          decoder.decode(
            new EncodedVideoChunk({ type: 'key', timestamp: 0, data: frame }),
          )
          decoder.decode(
            new EncodedVideoChunk({ type: 'key', timestamp: 1, data: frame }),
          )
          decoder.flush().then(
            () => finish(timeoutMeansBroken ? false : true),
            () => finish(false),
          )
        } catch {
          finish(false)
        }
      })
    })
    .catch(() => false)
}

/// A short, log-safe description of a decoder failure.
function describe(error: unknown): string {
  if (error instanceof Error) return `${error.name}: ${error.message}`
  return String(error)
}

export class VpxStreamDecoder {
  /// Reports a decoder failure once per failure; the client tunnels it to the
  /// host's log, which is the only way to see inside the WebView.
  onError: ((message: string) => void) | null = null
  private decoder: VideoDecoder | null = null
  private pending: Array<(frame: VideoFrame | null) => void> = []
  private timestamp = 0
  private broken = false
  /// A delta was dropped, so the reference chain is broken. Every later delta
  /// would decode into garbage until a keyframe re-anchors the stream, so the
  /// decoder stays idle (and the caller asks for a keyframe) instead.
  private needsKeyframe = false
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
    if (this.needsKeyframe) {
      if (!keyframe) return null
      // Rebuild the decoder on the keyframe: nothing from the broken chain
      // may survive into the new one.
      this.close()
    }
    if (this.decoder === null) {
      if (!keyframe) return null
      const decoder = new VideoDecoder({
        output: (frame) => {
          const resolve = this.pending.shift()
          if (resolve) resolve(frame)
          else frame.close()
        },
        error: (error) => {
          // Drop this decoder; the next keyframe builds a fresh one.
          this.onError?.(`${this.codec} decoder error: ${describe(error)}`)
          this.close()
        },
      })
      try {
        decoder.configure({
          codec: codecString(this.codec),
          optimizeForLatency: true,
        })
      } catch (error) {
        this.onError?.(
          `${this.codec} configure rejected for ${codecString(this.codec)}: ` +
            describe(error),
        )
        this.close()
        return null
      }
      this.decoder = decoder
    }
    const decoder = this.decoder
    if (decoder.state !== 'configured') {
      this.onError?.(`${this.codec} decoder state is ${decoder.state}`)
      return null
    }
    if (!keyframe && decoder.decodeQueueSize >= kMaxQueuedDeltas) {
      // Behind: dropping a delta costs a resync, but decoding the next ones on
      // top of a missing reference smears the picture until a keyframe. Stop
      // decoding until that keyframe arrives.
      this.dropped += 1
      this.needsKeyframe = true
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
    } catch (error) {
      this.onError?.(`${this.codec} decode threw: ${describe(error)}`)
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
    this.needsKeyframe = false
    this.dropped = 0
  }

  private settleAll(frame: VideoFrame | null): void {
    for (const resolve of this.pending) resolve(frame)
    this.pending = []
  }
}
