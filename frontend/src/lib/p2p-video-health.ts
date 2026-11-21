/// How long a host "stream-gap" notice explains a missing frame. The host
/// sends one (at most every 500 ms) when its own send queue refuses a frame.
/// Within this window a gap is a local backpressure symptom, so neither the
/// keyframe wait nor the video probe may blame the P2P path and force a relay
/// reconnect.
export const kBackpressureGapFreshMs = 2000

export interface P2pVideoHealth {
  now: number
  transportPath: string
  /** When the current stream keyframe wait started, 0 when not waiting. */
  streamKeyframeWaitStarted: number
  /** Last host stream-gap (reason=backpressure) notice, 0 when none. */
  lastBackpressureGapAt: number
  /** When a frame last arrived on the wire. */
  lastFrameResponseAt: number
  /** When the current video probe was sent, 0 before the first probe. */
  videoProbeAt: number
  /** How long to wait for a keyframe or a probe answer before giving up. */
  keyframeWaitMs: number
  /** Minimum gap between probes and the final fallback decision. */
  probeIntervalMs: number
}

export type P2pVideoAction = 'none' | 'probe' | 'fallback'
export type P2pVideoReason = 'keyframes' | 'probes' | ''

export interface P2pVideoAssessment {
  action: P2pVideoAction
  reason: P2pVideoReason
}

/// Decides whether a quiet P2P video path is a real path failure.
///
/// A recent host backpressure notice proves the media path is alive and
/// explains the missing reference locally, so it suppresses both the keyframe
/// timeout and the probe timeout. Without it, the two existing timeouts are
/// unchanged and still fall back to the fixed relay.
export function assessP2pVideo(health: P2pVideoHealth): P2pVideoAssessment {
  if (health.transportPath !== 'p2p') {
    return { action: 'none', reason: '' }
  }
  const backpressureFresh =
    health.lastBackpressureGapAt !== 0 &&
    health.now - health.lastBackpressureGapAt <= kBackpressureGapFreshMs
  if (backpressureFresh) {
    return { action: 'none', reason: '' }
  }
  if (
    health.streamKeyframeWaitStarted !== 0 &&
    health.now - health.streamKeyframeWaitStarted >= health.keyframeWaitMs
  ) {
    return { action: 'fallback', reason: 'keyframes' }
  }
  if (health.now - health.lastFrameResponseAt < health.keyframeWaitMs) {
    return { action: 'none', reason: '' }
  }
  if (health.videoProbeAt === 0) {
    return { action: 'probe', reason: '' }
  }
  return health.now - health.videoProbeAt >= health.probeIntervalMs
    ? { action: 'fallback', reason: 'probes' }
    : { action: 'none', reason: '' }
}
