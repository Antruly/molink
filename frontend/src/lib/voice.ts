import { PcmPlayer } from './audio'

/// Two-way voice for a call. Microphone PCM is captured at 48 kHz mono and
/// sent to the host; the host microphone is played through Web Audio. No
/// video, per the call design.
export type MicFailure =
  | 'unsupported'
  | 'denied'
  | 'missing'
  | 'busy'
  | 'error'

export class VoiceCall {
  private stream: MediaStream | null = null
  private context: AudioContext | null = null
  private source: MediaStreamAudioSourceNode | null = null
  private processor: ScriptProcessorNode | null = null
  private sink: GainNode | null = null
  private readonly player = new PcmPlayer(48000, 1, 1)
  private send: ((pcm: Uint8Array) => void) | null = null
  private muted = false

  async start(
    send: (pcm: Uint8Array) => void,
  ): Promise<{ mic: boolean; reason?: MicFailure }> {
    this.send = send
    // Playback is optional: a failed AudioContext must not hide the
    // microphone result (and vice versa).
    try {
      await this.player.start()
    } catch {
      // The host audio still arrives; it just cannot be played here.
    }
    let mic = true
    let reason: MicFailure | undefined
    // getUserMedia is absent on insecure origins (plain http:// to anything
    // other than localhost), which is how a remote browser usually reaches a
    // host. Say so instead of pretending the code failed.
    if (!navigator.mediaDevices?.getUserMedia) {
      mic = false
      reason = 'unsupported'
    } else {
      try {
        this.stream = await navigator.mediaDevices.getUserMedia({
          audio: {
            channelCount: 1,
            echoCancellation: true,
            noiseSuppression: true,
            autoGainControl: true,
          },
          video: false,
        })
      } catch (error) {
        mic = false
        const name = error instanceof DOMException ? error.name : ''
        if (name === 'NotAllowedError' || name === 'SecurityError') {
          reason = 'denied'
        } else if (
          name === 'NotFoundError' ||
          name === 'DevicesNotFoundError'
        ) {
          reason = 'missing'
        } else if (
          name === 'NotReadableError' ||
          name === 'TrackStartError' ||
          name === 'AbortError'
        ) {
          reason = 'busy'
        } else {
          reason = 'error'
        }
      }
    }
    if (this.stream) {
      try {
        const context = new AudioContext({ sampleRate: 48000 })
        this.context = context
        this.source = context.createMediaStreamSource(this.stream)
        this.processor = context.createScriptProcessor(2048, 1, 1)
        this.sink = context.createGain()
        this.sink.gain.value = 0
        this.processor.onaudioprocess = (event) => {
          if (this.muted || !this.send) return
          const input = event.inputBuffer.getChannelData(0)
          const pcm = new Uint8Array(input.length * 2)
          const view = new DataView(pcm.buffer)
          for (let i = 0; i < input.length; i += 1) {
            const sample = Math.max(-1, Math.min(1, input[i]))
            view.setInt16(
              i * 2,
              sample < 0 ? sample * 0x8000 : sample * 0x7fff,
              true,
            )
          }
          this.send(pcm)
        }
        this.source.connect(this.processor)
        this.processor.connect(this.sink)
        this.sink.connect(context.destination)
        if (context.state === 'suspended') {
          await context.resume().catch(() => undefined)
        }
      } catch {
        mic = false
        reason = 'error'
      }
    }
    return { mic, reason }
  }

  pushRemote(pcm: Uint8Array): void {
    this.player.push(pcm)
  }

  setMuted(muted: boolean): void {
    this.muted = muted
  }

  stop(): void {
    this.send = null
    this.processor?.disconnect()
    this.source?.disconnect()
    this.sink?.disconnect()
    this.stream?.getTracks().forEach((track) => track.stop())
    const context = this.context
    this.context = null
    if (context) void context.close().catch(() => undefined)
    this.player.stop()
    this.stream = null
    this.processor = null
    this.source = null
    this.sink = null
  }
}
