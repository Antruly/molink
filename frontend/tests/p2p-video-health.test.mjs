import assert from 'node:assert/strict'
import test from 'node:test'
import {
  assessP2pVideo,
  kBackpressureGapFreshMs,
} from '../src/lib/p2p-video-health.ts'

const waitMs = 6000
const probeIntervalMs = 3000

function health(overrides = {}) {
  return {
    now: 100_000,
    transportPath: 'p2p',
    streamKeyframeWaitStarted: 0,
    lastBackpressureGapAt: 0,
    lastFrameResponseAt: 100_000,
    videoProbeAt: 0,
    keyframeWaitMs: waitMs,
    probeIntervalMs,
    ...overrides,
  }
}

test('a fresh host backpressure notice suppresses the keyframe fallback', () => {
  const assessment = assessP2pVideo(
    health({
      // The keyframe wait is far past its deadline, but the host just said the
      // missing reference came from its own send queue.
      streamKeyframeWaitStarted: 100_000 - waitMs - 1,
      lastBackpressureGapAt: 100_000 - 500,
    }),
  )
  assert.deepEqual(assessment, { action: 'none', reason: '' })
})

test('a fresh backpressure notice also suppresses the quiet-link probe', () => {
  const assessment = assessP2pVideo(
    health({
      lastFrameResponseAt: 0,
      videoProbeAt: 50_000,
      lastBackpressureGapAt: 100_000 - 100,
    }),
  )
  assert.deepEqual(assessment, { action: 'none', reason: '' })
})

test('without a backpressure notice the keyframe wait still falls back', () => {
  const assessment = assessP2pVideo(
    health({
      streamKeyframeWaitStarted: 100_000 - waitMs - 1,
      lastBackpressureGapAt: 100_000 - kBackpressureGapFreshMs - 1,
    }),
  )
  assert.deepEqual(assessment, { action: 'fallback', reason: 'keyframes' })
})

test('a quiet link probes once, then falls back after the probe interval', () => {
  const quiet = health({ lastFrameResponseAt: 0 })
  assert.deepEqual(assessP2pVideo(quiet), { action: 'probe', reason: '' })
  assert.deepEqual(
    assessP2pVideo({ ...quiet, videoProbeAt: 100_000 - probeIntervalMs }),
    { action: 'fallback', reason: 'probes' },
  )
})

test('the relay path is never assessed for a P2P fallback', () => {
  assert.deepEqual(assessP2pVideo(health({ transportPath: 'relay' })), {
    action: 'none',
    reason: '',
  })
})
