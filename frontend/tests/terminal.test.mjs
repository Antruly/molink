// Terminal wire format: JSON control messages, binary frames, replay dedupe and
// the resize debounce. The panel itself is React; everything it sends and
// decodes is pure and lives in src/lib/terminal.ts.
import test from 'node:test'
import assert from 'node:assert/strict'

import {
  TERM_FRAME_INPUT,
  TERM_FRAME_OUTPUT,
  TERM_INPUT_HEADER_SIZE,
  TERM_OUTPUT_HEADER_SIZE,
  applyOutputFrame,
  createResizeDebouncer,
  decodeControl,
  decodeOutputFrame,
  encodeControl,
  encodeInputFrame,
  encodeOutputFrame,
  terminalAttach,
  terminalClose,
  terminalOpen,
  terminalResize,
  terminalSocketUrl,
} from '../src/lib/terminal.ts'

const bytes = (...values) => Uint8Array.from(values)
const text = (value) => new TextEncoder().encode(value)

function roundTrip(message) {
  return decodeControl(encodeControl(message))
}

test('control messages survive a JSON round trip', () => {
  assert.deepEqual(roundTrip(terminalOpen(80, 24)), {
    type: 'term-open',
    cols: 80,
    rows: 24,
  })
  assert.deepEqual(roundTrip(terminalOpen(120, 40, '/bin/bash')), {
    type: 'term-open',
    cols: 120,
    rows: 40,
    shell: '/bin/bash',
  })
  assert.deepEqual(roundTrip(terminalAttach(7, 4096)), {
    type: 'term-attach',
    id: 7,
    seq: 4096,
  })
  assert.deepEqual(roundTrip(terminalResize(7, 132, 43)), {
    type: 'term-resize',
    id: 7,
    cols: 132,
    rows: 43,
  })
  assert.deepEqual(roundTrip(terminalClose(7)), {
    type: 'term-close',
    id: 7,
  })
  assert.deepEqual(
    roundTrip({ type: 'term-exit', id: 7, code: 130 }),
    { type: 'term-exit', id: 7, code: 130 },
  )
  assert.deepEqual(
    roundTrip({ type: 'term-opened', id: 7 }),
    { type: 'term-opened', id: 7 },
  )
  assert.deepEqual(
    roundTrip({ type: 'term-attached', id: 7, seq: 4096, truncated: false }),
    { type: 'term-attached', id: 7, seq: 4096, truncated: false },
  )
  assert.deepEqual(
    roundTrip({ type: 'term-attached', id: 7, seq: 912, truncated: true }),
    { type: 'term-attached', id: 7, seq: 912, truncated: true },
  )
  assert.deepEqual(
    roundTrip({ type: 'error', code: 'terminal_disabled', message: 'off' }),
    { type: 'error', code: 'terminal_disabled', message: 'off' },
  )
  assert.deepEqual(
    roundTrip({ type: 'error', code: 'term_not_found', id: 9 }),
    { type: 'error', code: 'term_not_found', id: 9 },
  )
})

test('an empty shell is omitted, not sent as an empty string', () => {
  const message = roundTrip(terminalOpen(80, 24, ''))
  assert.equal('shell' in message, false)
})

test('unknown or malformed control messages decode to null', () => {
  assert.equal(decodeControl('not json'), null)
  assert.equal(decodeControl('null'), null)
  assert.equal(decodeControl('[]'), null)
  assert.equal(decodeControl('{"type":"term-future"}'), null)
  assert.equal(decodeControl('{"type":"term-resize","id":1,"cols":80}'), null)
  assert.equal(decodeControl('{"type":"term-exit","id":1}'), null)
  assert.equal(decodeControl('{"type":"error"}'), null)
  // term-attached needs a boolean truncated flag, not just an id and seq.
  assert.equal(decodeControl('{"type":"term-attached","id":1,"seq":2}'), null)
  assert.equal(
    decodeControl('{"type":"term-attached","id":1,"seq":2,"truncated":"yes"}'),
    null,
  )
})

test('errors name the terminal they belong to', () => {
  // The host puts the id on term_not_found (and resolve failures) so the page
  // can flag the matching tab instead of guessing from attach order.
  const named = decodeControl(
    '{"type":"error","code":"term_not_found","message":"terminal not found","id":9}',
  )
  assert.deepEqual(named, {
    type: 'error',
    code: 'term_not_found',
    message: 'terminal not found',
    id: 9,
  })
  // Handshake errors are not tied to a terminal and carry no id.
  const unnamed = decodeControl('{"type":"error","code":"terminal_disabled"}')
  assert.deepEqual(unnamed, { type: 'error', code: 'terminal_disabled' })
  assert.equal('id' in unnamed, false)
})

test('input frames carry kind 0x02, a little-endian u64 id and raw bytes', () => {
  const frame = encodeInputFrame(0x0102030405, bytes(0x68, 0x69, 0x0d))
  assert.equal(frame.length, TERM_INPUT_HEADER_SIZE + 3)
  assert.equal(frame[0], TERM_FRAME_INPUT)
  // Little-endian: the low byte comes first.
  assert.deepEqual(
    Array.from(frame.slice(1, 9)),
    [0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0x00, 0x00],
  )
  assert.deepEqual(Array.from(frame.slice(TERM_INPUT_HEADER_SIZE)), [
    0x68, 0x69, 0x0d,
  ])
})

test('output frames encode and decode the host layout', () => {
  const payload = text('hello\r\n')
  const frame = encodeOutputFrame(0x1_0000_0005, 0x2_0000_0003, payload)
  assert.equal(frame.length, TERM_OUTPUT_HEADER_SIZE + payload.length)
  assert.equal(frame[0], TERM_FRAME_OUTPUT)

  const decoded = decodeOutputFrame(frame)
  assert.notEqual(decoded, null)
  assert.equal(decoded.id, 0x1_0000_0005)
  assert.equal(decoded.seq, 0x2_0000_0003)
  assert.deepEqual(Array.from(decoded.data), Array.from(payload))
})

test('decodeOutputFrame works on a view with a non-zero byte offset', () => {
  const inner = encodeOutputFrame(3, 0, bytes(0x41))
  const backing = new Uint8Array(inner.length + 5)
  backing.set(inner, 5)
  const view = backing.subarray(5)
  const decoded = decodeOutputFrame(view)
  assert.equal(decoded.id, 3)
  assert.equal(decoded.seq, 0)
  assert.deepEqual(Array.from(decoded.data), [0x41])
})

test('decodeOutputFrame rejects short frames and the input kind', () => {
  assert.equal(decodeOutputFrame(bytes(1, 2, 3)), null)
  assert.equal(decodeOutputFrame(bytes(TERM_FRAME_INPUT, ...new Array(20).fill(0))), null)
  assert.equal(
    decodeOutputFrame(new Uint8Array(TERM_OUTPUT_HEADER_SIZE)),
    null,
  )
})

test('dedupe renders the first frame whole', () => {
  const result = applyOutputFrame(0, { seq: 0, data: text('abc') })
  assert.deepEqual(Array.from(result.data), Array.from(text('abc')))
  assert.equal(result.seq, 3)
})

test('dedupe drops a frame that was already rendered', () => {
  assert.equal(applyOutputFrame(3, { seq: 0, data: text('abc') }), null)
  assert.equal(applyOutputFrame(5, { seq: 1, data: text('abcd') }), null)
})

test('term-attach replay: only the bytes since seq are written again', () => {
  // The page rendered "abc" (seq 3); the host replays from 1 because the
  // attach carried seq=3 but a live frame raced the replay.
  const result = applyOutputFrame(3, { seq: 1, data: text('bcd') })
  assert.deepEqual(Array.from(result.data), Array.from(text('d')))
  assert.equal(result.seq, 4)
})

test('dedupe accepts a replay that starts after the requested seq', () => {
  // The 1 MiB ring had already evicted the requested offset; the host starts
  // at its oldest byte. There is a gap, but the page still shows the tail.
  const result = applyOutputFrame(4, { seq: 100, data: text('xy') })
  assert.deepEqual(Array.from(result.data), Array.from(text('xy')))
  assert.equal(result.seq, 102)
})

test('dedupe ignores empty frames and keeps the byte counter monotonic', () => {
  assert.equal(applyOutputFrame(8, { seq: 8, data: new Uint8Array() }), null)
  assert.equal(applyOutputFrame(8, { seq: 9, data: text('z') }).seq, 10)
})

function fakeTimers() {
  let now = 0
  let nextId = 1
  const jobs = new Map()
  return {
    timers: {
      setTimeout(callback, delay) {
        const id = nextId++
        jobs.set(id, { callback, at: now + delay })
        return id
      },
      clearTimeout(id) {
        jobs.delete(id)
      },
    },
    advance(ms) {
      now += ms
      for (const [id, job] of [...jobs]) {
        if (job.at <= now) {
          jobs.delete(id)
          job.callback()
        }
      }
    },
    pending() {
      return jobs.size
    },
  }
}

test('consecutive resizes only send the last size after 200 ms', () => {
  const clock = fakeTimers()
  const sent = []
  const debouncer = createResizeDebouncer(
    (cols, rows) => sent.push([cols, rows]),
    { timers: clock.timers },
  )

  debouncer.update(80, 24)
  debouncer.update(100, 30)
  debouncer.update(120, 40)
  clock.advance(199)
  assert.deepEqual(sent, [])
  clock.advance(1)
  assert.deepEqual(sent, [[120, 40]])
  assert.equal(clock.pending(), 0)
})

test('a resize equal to the last one sent schedules nothing', () => {
  const clock = fakeTimers()
  const sent = []
  const debouncer = createResizeDebouncer(
    (cols, rows) => sent.push([cols, rows]),
    { timers: clock.timers },
  )

  debouncer.update(90, 25)
  clock.advance(200)
  assert.deepEqual(sent, [[90, 25]])

  debouncer.update(90, 25)
  assert.equal(debouncer.pending(), null)
  assert.equal(clock.pending(), 0)
  clock.advance(500)
  assert.deepEqual(sent, [[90, 25]])
})

test('flush sends the pending size immediately and cancels the timer', () => {
  const clock = fakeTimers()
  const sent = []
  const debouncer = createResizeDebouncer(
    (cols, rows) => sent.push([cols, rows]),
    { timers: clock.timers },
  )

  debouncer.update(101, 31)
  debouncer.flush()
  assert.deepEqual(sent, [[101, 31]])
  assert.equal(clock.pending(), 0)
  clock.advance(500)
  assert.deepEqual(sent, [[101, 31]])
})

test('cancel drops the pending size', () => {
  const clock = fakeTimers()
  const sent = []
  const debouncer = createResizeDebouncer(
    (cols, rows) => sent.push([cols, rows]),
    { timers: clock.timers },
  )

  debouncer.update(60, 20)
  debouncer.cancel()
  clock.advance(500)
  assert.deepEqual(sent, [])
  assert.equal(debouncer.pending(), null)
})

test('invalid dimensions are ignored', () => {
  const clock = fakeTimers()
  const sent = []
  const debouncer = createResizeDebouncer(
    (cols, rows) => sent.push([cols, rows]),
    { timers: clock.timers },
  )

  debouncer.update(0, 24)
  debouncer.update(80, -1)
  assert.equal(clock.pending(), 0)
  clock.advance(500)
  assert.deepEqual(sent, [])
})

test('the default debounce interval is 200 ms', () => {
  const delays = []
  const timers = {
    setTimeout(_callback, delay) {
      delays.push(delay)
      return 1
    },
    clearTimeout() {},
  }
  const debouncer = createResizeDebouncer(() => {}, { timers })
  debouncer.update(80, 24)
  assert.deepEqual(delays, [200])
})

test('the terminal URL follows the page scheme and escapes the token', () => {
  assert.equal(
    terminalSocketUrl('192.168.2.10', 28418, 'http:', 'abc'),
    'ws://192.168.2.10:28418/ws/terminal?token=abc',
  )
  assert.equal(
    terminalSocketUrl('host.example', 443, 'https:', 'a b&c'),
    'wss://host.example:443/ws/terminal?token=a+b%26c',
  )
  assert.equal(
    terminalSocketUrl('fe80::1', 28418, 'http:', 't'),
    'ws://[fe80::1]:28418/ws/terminal?token=t',
  )
  assert.equal(
    terminalSocketUrl('[fe80::1]', 28418, 'http:', 't'),
    'ws://[fe80::1]:28418/ws/terminal?token=t',
  )
})
