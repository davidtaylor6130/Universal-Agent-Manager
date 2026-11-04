import { describe, expect, it } from 'vitest'
import { sanitizeCppAppState, sanitizeDeletionProgress } from './sanitizers'

describe('deletion progress contract', () => {
  it('keeps well-formed progress and drops the optional fields when absent', () => {
    expect(sanitizeDeletionProgress({ requestId: 'r1', completed: 3, total: 12, currentLabel: 'alpha' }))
      .toEqual({ requestId: 'r1', completed: 3, total: 12, currentLabel: 'alpha' })
    expect(sanitizeDeletionProgress({ completed: 0, total: 1 })).toEqual({ completed: 0, total: 1 })
  })

  it.each([
    undefined,
    null,
    'x',
    {},
    { completed: 1 },
    { completed: NaN, total: 2 },
    { completed: 1, total: Infinity },
    { completed: -1, total: 2 },
    { completed: 1, total: -2 },
    { completed: 3, total: 2 },
    { completed: '1', total: '2' },
  ])('rejects invalid progress %j', (value) => {
    expect(sanitizeDeletionProgress(value)).toBeUndefined()
  })

  it('passes progress through the full app state sanitizer', () => {
    expect(sanitizeCppAppState({ deletionProgress: { completed: 2, total: 5 } })?.deletionProgress)
      .toEqual({ completed: 2, total: 5 })
    expect(sanitizeCppAppState({ deletionProgress: { completed: 9, total: 5 } })?.deletionProgress).toBeUndefined()
    expect(sanitizeCppAppState({})?.deletionProgress).toBeUndefined()
  })
})
