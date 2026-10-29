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

/// Splits an Annex-B access unit into NAL units with the start codes removed.
function splitNalUnits(bytes: Uint8Array): Uint8Array[] {
  const nals: Uint8Array[] = []
  let start = -1
  let i = 0
  while (i + 2 < bytes.length) {
    if (bytes[i] === 0 && bytes[i + 1] === 0 && bytes[i + 2] === 1) {
      if (start >= 0) nals.push(bytes.subarray(start, i))
      i += 3
      start = i
      continue
    }
    i += 1
  }
  if (start >= 0 && start < bytes.length) nals.push(bytes.subarray(start))
  return nals.filter((nal) => nal.length > 0)
}

const kNalSps = 7
const kNalPps = 8

/// `AVCDecoderConfigurationRecord` (ISO/IEC 14496-15 §5.2.4.1) built from one
/// SPS and one PPS.
///
/// WebCodecs hands the decoder AVCC (length-prefixed NAL units) unless the
/// config carries `description`. Chrome has a private `avc: {format:'annexb'}`
/// extension for start codes; WebKit does not implement it and silently ignores
/// the unknown member, so an Annex-B feed to WebKit decodes garbage and the
/// picture freezes on the last tile frame. Building the record works in both.
function avcDescription(sps: Uint8Array, pps: Uint8Array): Uint8Array {
  const out = new Uint8Array(11 + sps.length + pps.length)
  out[0] = 1                     // configurationVersion
  out[1] = sps[1] ?? 0x42        // AVCProfileIndication
  out[2] = sps[2] ?? 0xe0        // profile_compatibility
  out[3] = sps[3] ?? 0x1e        // AVCLevelIndication
  out[4] = 0xff                  // 4-byte NAL lengths
  out[5] = 0xe1                  // one SPS
  out[6] = (sps.length >> 8) & 0xff
  out[7] = sps.length & 0xff
  out.set(sps, 8)
  const ppsAt = 8 + sps.length
  out[ppsAt] = 1                 // one PPS
  out[ppsAt + 1] = (pps.length >> 8) & 0xff
  out[ppsAt + 2] = pps.length & 0xff
  out.set(pps, ppsAt + 3)
  return out
}

/// AVCC access unit: every NAL prefixed with its 4-byte big-endian length.
function toAvcc(nals: Uint8Array[]): Uint8Array {
  let size = 0
  for (const nal of nals) size += 4 + nal.length
  const out = new Uint8Array(size)
  let at = 0
  for (const nal of nals) {
    out[at] = (nal.length >>> 24) & 0xff
    out[at + 1] = (nal.length >>> 16) & 0xff
    out[at + 2] = (nal.length >>> 8) & 0xff
    out[at + 3] = nal.length & 0xff
    out.set(nal, at + 4)
    at += 4 + nal.length
  }
  return out
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
    const nals = splitNalUnits(bytes)
    if (nals.length === 0) return null
    if (this.decoder === null) {
      if (!keyframe) return null   // a delta has nothing to reference yet
      const codec = h264CodecString(bytes)
      if (codec === null) return null
      const sps = nals.find((nal) => (nal[0]! & 0x1f) === kNalSps)
      const pps = nals.find((nal) => (nal[0]! & 0x1f) === kNalPps)
      // A keyframe that does not carry the parameter sets cannot configure a
      // decoder; ask for another one instead of feeding it blind.
      if (sps === undefined || pps === undefined) return null
      const decoder = new VideoDecoder({
        output: (frame) => {
          const resolve = this.pending.shift()
          if (resolve) resolve(frame)
          else frame.close()
        },
        error: () => {
          // Drop this decoder and let the next keyframe build a fresh one.
          // Marking the stream broken for the rest of the session turned one
          // bad picture into a permanently frozen canvas.
          this.close()
        },
      })
      try {
        // `description` makes the decoder read AVCC (length-prefixed) chunks,
        // which is what every browser implements.
        decoder.configure({
          codec,
          description: avcDescription(sps, pps),
          optimizeForLatency: true,
        })
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
          data: toAvcc(nals),
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
