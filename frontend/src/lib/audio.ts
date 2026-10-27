/// Plays interleaved 16-bit PCM from the remote host through Web Audio.
///
/// The host sends one packet per loopback read; buffers are scheduled ahead on
/// the AudioContext clock. A growing backlog means the link is faster than
/// playback, so the queue is trimmed instead of letting latency accumulate.
export class PcmPlayer {
  private context: AudioContext | null = null
  private gain: GainNode | null = null
  private nextTime = 0
  private volume: number

  constructor(
    private readonly sampleRate: number,
    private readonly channels: number,
    volume = 0.8,
  ) {
    this.volume = Math.min(1, Math.max(0, volume))
  }

  async start(): Promise<void> {
    if (this.context) return
    const context = new AudioContext({ sampleRate: this.sampleRate })
    this.context = context
    this.gain = context.createGain()
    this.gain.gain.value = this.volume
    this.gain.connect(context.destination)
    this.nextTime = context.currentTime + 0.08
    if (context.state === 'suspended') {
      await context.resume().catch(() => undefined)
    }
  }

  push(pcm: Uint8Array): void {
    const context = this.context
    const gain = this.gain
    if (!context || !gain || pcm.byteLength < 2) return
    // A hidden or inactive page can leave the context suspended; without this
    // the stream would schedule buffers into a paused clock and stay silent.
    if (context.state === 'suspended') {
      void context.resume().catch(() => undefined)
    }
    const channels = Math.max(1, this.channels)
    const frames = Math.floor(pcm.byteLength / 2 / channels)
    if (frames <= 0) return
    const view = new DataView(pcm.buffer, pcm.byteOffset, pcm.byteLength)
    const buffer = context.createBuffer(channels, frames, this.sampleRate)
    for (let channel = 0; channel < channels; channel += 1) {
      const target = buffer.getChannelData(channel)
      for (let frame = 0; frame < frames; frame += 1) {
        const offset = (frame * channels + channel) * 2
        target[frame] = view.getInt16(offset, true) / 32768
      }
    }
    const source = context.createBufferSource()
    source.buffer = buffer
    source.connect(gain)
    const now = context.currentTime
    if (this.nextTime < now + 0.02) this.nextTime = now + 0.05
    // More than a second queued means packets arrived late; drop the backlog
    // so audio stays close to the picture.
    if (this.nextTime > now + 1.0) this.nextTime = now + 0.1
    source.start(this.nextTime)
    this.nextTime += buffer.duration
  }

  setVolume(value: number): void {
    this.volume = Math.min(1, Math.max(0, value))
    if (this.gain) this.gain.gain.value = this.volume
  }

  stop(): void {
    const context = this.context
    this.context = null
    this.gain = null
    this.nextTime = 0
    if (context) void context.close().catch(() => undefined)
  }
}
