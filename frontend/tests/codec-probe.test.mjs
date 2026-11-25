import assert from 'node:assert/strict'
import test from 'node:test'
import {
  kCodecProbeAttempts,
  kCodecProbeRetryDelayMs,
  parseForcedCodec,
  probeWithRetry,
  vp9CodecString,
  vp9ProfileFromFrame,
} from '../src/lib/vpx-stream.ts'

// The probe keyframe from vpx-stream.ts begins with the VP9 marker (0b10) and
// profile bits 0, i.e. profile 0.
const kProbeVp9Header = Uint8Array.from(
  Buffer.from(
    'gkmDQiAD8AP2CDgkHBhCAAIAb7F4rp+Zq9pZ1/P7ZNQ/H1EDPnzUav38aI69oSOAAAB+',
    'base64',
  ),
)

test('vp9 codec strings name profile, level and bit depth', () => {
  assert.equal(vp9CodecString(0), 'vp09.00.10.08')
  assert.equal(vp9CodecString(2), 'vp09.02.10.10')
})

test('the VP9 profile is read from the frame header', () => {
  assert.equal(vp9ProfileFromFrame(kProbeVp9Header), 0)
  // 0b10 marker, profile_high_bit set, profile_low_bit clear.
  assert.equal(vp9ProfileFromFrame(Uint8Array.of(0x90)), 2)
  // Not a VP9 frame marker.
  assert.equal(vp9ProfileFromFrame(Uint8Array.of(0x00)), null)
  assert.equal(vp9ProfileFromFrame(new Uint8Array(0)), null)
})

test('the retry schedule is three attempts thirty seconds apart', () => {
  assert.equal(kCodecProbeAttempts, 3)
  assert.equal(kCodecProbeRetryDelayMs, 30_000)
})

test('one failed probe is retried instead of closing the codec', async () => {
  const delays = []
  let calls = 0
  const run = await probeWithRetry(
    async () => {
      calls += 1
      return calls === 1
        ? { ok: false, reason: 'first frame lost' }
        : { ok: true }
    },
    {
      sleep: async (ms) => {
        delays.push(ms)
      },
    },
  )
  assert.deepEqual(run, { ok: true, attempts: 2, reason: '' })
  assert.deepEqual(delays, [30_000])
})

test('three failed probes close the codec and log every attempt', async () => {
  const delays = []
  const failures = []
  const run = await probeWithRetry(
    async () => ({ ok: false, reason: 'Decode error' }),
    {
      sleep: async (ms) => {
        delays.push(ms)
      },
      onFailure: (attempt, reason) => failures.push({ attempt, reason }),
    },
  )
  assert.deepEqual(run, { ok: false, attempts: 3, reason: 'Decode error' })
  assert.deepEqual(delays, [30_000, 30_000])
  assert.deepEqual(
    failures.map((entry) => entry.attempt),
    [1, 2, 3],
  )
})

test('a probe that throws counts as a failed attempt', async () => {
  const run = await probeWithRetry(
    async () => {
      throw new Error('decoder crashed')
    },
    {
      maxAttempts: 1,
      sleep: async () => {},
    },
  )
  assert.equal(run.ok, false)
  assert.equal(run.attempts, 1)
  assert.match(run.reason, /decoder crashed/)
})

test('?codec= forces a known codec and ignores the rest', () => {
  assert.equal(parseForcedCodec('?codec=vp9'), 'vp9')
  assert.equal(parseForcedCodec('?codec=vp8'), 'vp8')
  assert.equal(parseForcedCodec('?codec=av1'), 'av1')
  assert.equal(parseForcedCodec('?codec=h264'), 'h264')
  assert.equal(parseForcedCodec('?codec=tiles'), null)
  assert.equal(parseForcedCodec('?codec='), null)
  assert.equal(parseForcedCodec(''), null)
})
