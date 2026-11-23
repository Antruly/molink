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
  ADVANCED_BITRATE_MAX,
  ADVANCED_DEBOUNCE_MS,
  ADVANCED_FPS_MAX,
  ADVANCED_FPS_MIN,
  ADVANCED_QUALITY_MAX,
  DEFAULT_SESSION_PRESET,
  PRESET_PLANS,
  SESSION_PRESETS,
  advancedFormatForChroma,
  advancedPresetMessage,
  advancedQualityHint,
  advancedStorageKey,
  codecExitsTruecolor,
  createAdvancedPresetPump,
  defaultAdvancedParams,
  effectivePresetChroma,
  isExplicitStreamCodec,
  normalizeAdvancedParams,
  normalizePresetId,
  parseStoredAdvanced,
  parseStoredPreset,
  presetMessage,
  presetPlan,
  presetStorageKey,
  readStoredAdvanced,
  readStoredPreset,
  viewerPresetAfterReply,
  writeStoredAdvanced,
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
  assert.equal(hd.chroma, '420')
  assert.equal(hd.format, 'preset')
  assert.equal(hd.codec, 'auto')

  const truecolor = presetMessage('truecolor', 18)
  assert.equal(truecolor.format, 'bgra')
  assert.equal(truecolor.chroma, '444')
  assert.equal(truecolor.rate_mode, 'cq')

  // A lowered rate is sent as-is; the preset itself stays the viewer's.
  const lowered = presetMessage('balanced', 12)
  assert.equal(lowered.preset, 'balanced')
  assert.equal(lowered.fps, 12)
})

test('switching tiers rebuilds every wire field (truecolor -> auto)', () => {
  const truecolor = presetMessage('truecolor', 18)
  const auto = presetMessage('auto', 30)
  // Auto must not inherit truecolour's 4:4:4/BGRA/constant-quality request.
  assert.equal(truecolor.chroma, '444')
  assert.equal(auto.chroma, 'auto')
  assert.equal(auto.format, 'preset')
  assert.equal(auto.rate_mode, 'auto')
  assert.equal(auto.bitrate_kbps, 0)
  assert.equal(auto.preset, 'auto')
  // The colour tiers stream 4:2:0; only truecolour asks for 4:4:4.
  for (const id of ['turbo', 'smooth', 'balanced', 'hd', 'auto']) {
    assert.notEqual(presetPlan(id).chroma, '444')
  }
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

test('an explicit encoder locks the stream and forces 4:2:0', () => {
  for (const codec of ['av1', 'h264', 'vp9', 'vp8']) {
    assert.equal(isExplicitStreamCodec(codec), true)
    assert.equal(effectivePresetChroma('444', codec), '420')
    assert.equal(codecExitsTruecolor('truecolor', codec), true)
  }
  assert.equal(isExplicitStreamCodec('auto'), false)
  assert.equal(isExplicitStreamCodec('tiles'), false)
  // Auto/tiles keep the tier's own chroma and never warn about truecolour.
  assert.equal(effectivePresetChroma('444', 'auto'), '444')
  assert.equal(effectivePresetChroma('420', 'auto'), '420')
  assert.equal(codecExitsTruecolor('truecolor', 'auto'), false)
  assert.equal(codecExitsTruecolor('balanced', 'vp9'), false)
})

test('advanced defaults come from the selected preset tier', () => {
  const hd = defaultAdvancedParams('hd')
  assert.deepEqual(hd, {
    fps: 45,
    quality: 0,
    bitrateKbps: 0,
    rateMode: 'auto',
    format: 'preset',
    chroma: '420',
    codec: 'auto',
  })
  const truecolor = defaultAdvancedParams('truecolor')
  assert.equal(truecolor.fps, 18)
  assert.equal(truecolor.format, 'bgra')
  assert.equal(truecolor.chroma, '444')
  assert.equal(truecolor.rateMode, 'cq')
  // The advanced range is the documented 5..60, 0..63 and 0..50000.
  assert.equal(ADVANCED_FPS_MIN, 5)
  assert.equal(ADVANCED_FPS_MAX, 60)
  assert.equal(ADVANCED_QUALITY_MAX, 63)
  assert.equal(ADVANCED_BITRATE_MAX, 50000)
})

test('advanced parameters round-trip per device and bad values fall back', () => {
  const host = '192.168.2.20'
  const port = 28418
  const params = {
    fps: 20,
    quality: 40,
    bitrateKbps: 8000,
    rateMode: 'cbr',
    format: 'preset',
    chroma: '420',
    codec: 'vp9',
  }
  writeStoredAdvanced(host, port, params)
  assert.deepEqual(readStoredAdvanced(host, port), params)
  assert.equal(
    advancedStorageKey(' 192.168.2.20 ', 28418),
    'molink.advanced.192.168.2.20:28418',
  )
  // A hand-edited file must not break the panel: numbers clamp, enums fall
  // back one field at a time.
  const repaired = parseStoredAdvanced(
    '{"fps":999,"quality":-3,"bitrateKbps":99999,"rateMode":"vbr","chroma":"422","codec":"hevc"}',
  )
  assert.equal(repaired.fps, ADVANCED_FPS_MAX)
  assert.equal(repaired.quality, 0)
  assert.equal(repaired.bitrateKbps, ADVANCED_BITRATE_MAX)
  assert.equal(repaired.rateMode, 'auto')
  assert.equal(repaired.chroma, '420')
  assert.equal(repaired.codec, 'auto')
  assert.equal(parseStoredAdvanced('not json'), null)
  assert.equal(normalizeAdvancedParams({ fps: 1 }).fps, ADVANCED_FPS_MIN)
})

test('the custom message carries the panel values and resolves chroma', () => {
  const message = advancedPresetMessage({
    fps: 20,
    quality: 40,
    bitrateKbps: 8000,
    rateMode: 'cbr',
    format: 'preset',
    chroma: '420',
    codec: 'vp9',
  })
  assert.deepEqual(message, {
    type: 'session-preset',
    preset: 'custom',
    fps: 20,
    quality: 40,
    bitrate_kbps: 8000,
    rate_mode: 'cbr',
    format: 'preset',
    chroma: '420',
    codec: 'vp9',
  })

  // 4:4:4 means the pixel-exact BGRA tile path.
  const base = {
    fps: 20,
    quality: 40,
    bitrateKbps: 8000,
    rateMode: 'cbr',
    format: 'preset',
    chroma: '420',
    codec: 'auto',
  }
  const four = advancedPresetMessage({ ...base, chroma: '444' })
  assert.equal(four.preset, 'custom')
  assert.equal(four.chroma, '444')
  assert.equal(four.format, 'bgra')
  assert.equal(
    advancedQualityHint({ ...base, format: 'bgra', chroma: '444' }),
    'tiles',
  )

  // A pinned stream encoder is 4:2:0: the UI says so and the request is sent
  // as 4:2:0 rather than asking for an impossible 4:4:4 stream.
  const pinned = { ...base, chroma: '444', codec: 'vp9' }
  assert.equal(advancedPresetMessage(pinned).chroma, '420')
  assert.equal(advancedPresetMessage(pinned).format, 'preset')
  assert.equal(advancedQualityHint(pinned), 'forced420')

  // Choosing tiles explicitly warns even at 4:2:0.
  const tiles = { ...base, chroma: '420', codec: 'tiles' }
  assert.equal(advancedPresetMessage(tiles).codec, 'tiles')
  assert.equal(advancedQualityHint(tiles), 'tiles')
  assert.equal(advancedQualityHint({ ...base, codec: 'av1' }), null)

  assert.equal(advancedFormatForChroma('gray'), 'gray8')
  assert.equal(advancedFormatForChroma('565'), 'rgb565')
  assert.equal(advancedFormatForChroma('420', 'bgra'), 'preset')
})

test('returning to a preset rebuilds every field (custom cannot leak)', () => {
  const custom = advancedPresetMessage({
    ...defaultAdvancedParams('truecolor'),
    chroma: '444',
    format: 'bgra',
    codec: 'auto',
  })
  const hd = presetMessage('hd', 45)
  assert.equal(custom.preset, 'custom')
  assert.equal(custom.chroma, '444')
  assert.equal(hd.preset, 'hd')
  assert.equal(hd.chroma, '420')
  assert.equal(hd.format, 'preset')
  assert.equal(hd.rate_mode, 'auto')
  assert.equal(hd.bitrate_kbps, 0)
})

test('the advanced panel debounces rapid slider changes for 300ms', () => {
  const scheduled = []
  const sent = []
  let nextHandle = 1
  const timers = {
    set(callback, ms) {
      const handle = nextHandle++
      scheduled.push({ handle, callback, ms, cancelled: false })
      return handle
    },
    clear(handle) {
      const entry = scheduled.find((item) => item.handle === handle)
      if (entry) entry.cancelled = true
    },
  }
  const pump = createAdvancedPresetPump(
    (message) => sent.push(message.fps),
    ADVANCED_DEBOUNCE_MS,
    timers,
  )
  const message = (fps) => ({ ...advancedPresetMessage(defaultAdvancedParams()), fps })
  pump.update(message(12))
  pump.update(message(33))
  pump.update(message(45))
  assert.equal(scheduled.length, 3)
  assert.equal(scheduled[0].cancelled, true)
  assert.equal(scheduled[1].cancelled, true)
  assert.equal(scheduled[2].ms, 300)
  scheduled[2].callback()
  assert.deepEqual(sent, [45])

  pump.update(message(20))
  pump.cancel()
  assert.equal(scheduled[3].cancelled, true)
  scheduled[3].callback()
  assert.deepEqual(sent, [45], 'a cancelled drag sends nothing')
})
