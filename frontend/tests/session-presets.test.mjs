import assert from 'node:assert/strict'
import test from 'node:test'

// The preset helpers touch localStorage lazily, so a tiny fake is enough. It
// must exist before the storage tests run, not before the module is imported.
const memory = new Map()
globalThis.window = {
  localStorage: {
    get length() {
      return memory.size
    },
    key(index) {
      return [...memory.keys()][index] ?? null
    },
    getItem(key) {
      return memory.has(key) ? memory.get(key) : null
    },
    setItem(key, value) {
      memory.set(key, String(value))
    },
  },
}

const {
  DEFAULT_SESSION_PRESET,
  PRESET_PLANS,
  SESSION_PRESETS,
  normalizePresetId,
  parseStoredPreset,
  presetMessage,
  presetPlan,
  presetStorageKey,
  readStoredPreset,
  viewerPresetAfterReply,
  writeStoredPreset,
} = await import('../src/lib/session-presets.ts')

test('the five fixed tiers and auto are the only presets', () => {
  assert.deepEqual([...SESSION_PRESETS], [
    'turbo',
    'smooth',
    'balanced',
    'hd',
    'truecolor',
    'auto',
  ])
  assert.equal(DEFAULT_SESSION_PRESET, 'smooth')
})

test('each preset maps to one fixed quality mode and rate', () => {
  assert.equal(presetPlan('turbo').mode, 'turbo')
  assert.equal(presetPlan('turbo').fps, 10)
  assert.equal(presetPlan('smooth').mode, 'smooth')
  assert.equal(presetPlan('smooth').fps, 24)
  assert.equal(presetPlan('balanced').mode, 'balanced')
  assert.equal(presetPlan('balanced').fps, 30)
  assert.equal(presetPlan('hd').mode, 'quality')
  assert.equal(presetPlan('hd').fps, 45)
  assert.equal(presetPlan('truecolor').mode, 'lossless')
  assert.equal(presetPlan('truecolor').fps, 18)
  assert.equal(presetPlan('truecolor').format, 'bgra')
  assert.equal(presetPlan('truecolor').chroma, '444')
  // Auto is the only tier that may adapt.
  assert.equal(presetPlan('auto').mode, 'balanced')
  for (const id of ['turbo', 'smooth', 'balanced', 'hd', 'truecolor']) {
    assert.notEqual(PRESET_PLANS[id].mode, 'auto')
  }
})

test('the persisted key is per host:port', () => {
  assert.equal(
    presetStorageKey('192.168.2.20', 28418),
    'molink.preset.192.168.2.20:28418',
  )
  assert.equal(
    presetStorageKey(' molink.ssvr.top ', 443),
    'molink.preset.molink.ssvr.top:443',
  )
})

test('a saved tier round-trips and unknown values fall back', () => {
  const host = '192.168.2.20'
  const port = 28418
  writeStoredPreset(host, port, { preset: 'truecolor', updatedAt: 7 })
  const stored = readStoredPreset(host, port)
  assert.equal(stored?.preset, 'truecolor')
  assert.equal(stored?.updatedAt, 7)

  // A hand-edited or older value must not break the session.
  assert.equal(parseStoredPreset('{"preset":"8k"}')?.preset, 'smooth')
  assert.equal(parseStoredPreset('not json'), null)
  assert.equal(normalizePresetId('nonsense'), 'smooth')
  assert.equal(normalizePresetId(undefined), 'smooth')
})

test('the wire message carries the selected preset, not a host default', () => {
  const hd = presetMessage('hd', 45)
  assert.equal(hd.type, 'session-preset')
  assert.equal(hd.preset, 'hd')
  assert.equal(hd.fps, 45)
  assert.equal(hd.quality, 0)
  assert.equal(hd.chroma, '444')
  assert.equal(hd.format, 'preset')
  assert.equal(hd.codec, 'auto')

  const truecolor = presetMessage('truecolor', 18)
  assert.equal(truecolor.format, 'bgra')
  assert.equal(truecolor.rate_mode, 'cq')

  // A lowered rate is sent as-is; the preset itself stays the viewer's.
  const lowered = presetMessage('balanced', 12)
  assert.equal(lowered.preset, 'balanced')
  assert.equal(lowered.fps, 12)
})

test('a host reply never moves the viewer selector', () => {
  // The host echoes something else (an older host's default, or a reply for
  // another viewer). Only the actual-values line may change.
  const selected = 'truecolor'
  assert.equal(
    viewerPresetAfterReply(selected, { preset: 'balanced' }),
    selected,
  )
  assert.equal(
    viewerPresetAfterReply(selected, { preset: 'truecolor' }),
    selected,
  )
  assert.equal(viewerPresetAfterReply(selected, null), selected)
  assert.equal(viewerPresetAfterReply(selected, {}), selected)
})
