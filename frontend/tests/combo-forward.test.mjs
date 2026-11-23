import assert from 'node:assert/strict'
import test from 'node:test'
import {
  comboForwardAnswer,
  normalizeComboForwardAsked,
  shouldAskComboForward,
} from '../src/lib/combo-forward.ts'

const base = {
  applicable: true,
  enabled: false,
  asked: null,
  answeredThisSession: false,
}

test('the first combo asks when forwarding is off and nothing is saved', () => {
  assert.equal(shouldAskComboForward(base), true)
  // A plain browser has no grab to enable, so it must not prompt.
  assert.equal(shouldAskComboForward({ ...base, applicable: false }), false)
  // Already on: nothing to ask.
  assert.equal(shouldAskComboForward({ ...base, enabled: true }), false)
})

test('a remembered answer never asks again', () => {
  assert.equal(
    shouldAskComboForward({ ...base, asked: 'enabled' }),
    false,
  )
  assert.equal(
    shouldAskComboForward({ ...base, asked: 'declined' }),
    false,
  )
  assert.equal(normalizeComboForwardAsked('enabled'), 'enabled')
  assert.equal(normalizeComboForwardAsked('declined'), 'declined')
  assert.equal(normalizeComboForwardAsked('yes'), null)
  assert.equal(normalizeComboForwardAsked(null), null)
})

test('remembering enable turns grab on and suppresses the prompt', () => {
  const answer = comboForwardAnswer(true, true)
  assert.equal(answer.enabled, true)
  assert.equal(answer.asked, 'enabled')
  assert.equal(answer.answeredThisSession, true)
  assert.equal(
    shouldAskComboForward({
      ...base,
      enabled: answer.enabled,
      asked: answer.asked,
      answeredThisSession: answer.answeredThisSession,
    }),
    false,
  )
})

test('remembering decline suppresses the prompt but leaves grab off', () => {
  const answer = comboForwardAnswer(false, true)
  assert.equal(answer.enabled, false)
  assert.equal(answer.asked, 'declined')
  assert.equal(
    shouldAskComboForward({ ...base, answeredThisSession: true, asked: answer.asked }),
    false,
  )
})

test('without remember this session is quiet but the next one asks again', () => {
  const answer = comboForwardAnswer(false, false)
  assert.equal(answer.asked, null)
  // Same page session: no repeat prompt.
  assert.equal(
    shouldAskComboForward({
      ...base,
      enabled: answer.enabled,
      asked: answer.asked,
      answeredThisSession: true,
    }),
    false,
  )
  // Next launch resets the in-memory flag and asks again.
  assert.equal(
    shouldAskComboForward({
      ...base,
      enabled: answer.enabled,
      asked: answer.asked,
      answeredThisSession: false,
    }),
    true,
  )
})
