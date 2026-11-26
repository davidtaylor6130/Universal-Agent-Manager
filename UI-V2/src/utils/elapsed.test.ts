import { describe, expect, it } from 'vitest'
import { formatElapsed } from './elapsed'

describe('formatElapsed', () => {
  it.each([
    [0, '0s'],
    [45_000, '45s'],
    [60_000, '1m 0s'],
    [65_000, '1m 5s'],
    [3_599_000, '59m 59s'],
    [3_600_000, '1h 0m'],
    [3_725_000, '1h 2m'],
    [26 * 3_600_000 + 5 * 60_000, '26h 5m'],
  ])('formats %i ms as %s', (ms, text) => {
    expect(formatElapsed(ms)).toBe(text)
  })

  it('rounds to the nearest second, so a live clock and the saved duration agree', () => {
    expect(formatElapsed(1_499)).toBe('1s')
    expect(formatElapsed(1_500)).toBe('2s')
    expect(formatElapsed(59_600)).toBe('1m 0s')
  })

  it('drops seconds from one minute on for dense rows and keeps the hour format', () => {
    expect(formatElapsed(59_000, { seconds: 'under-minute' })).toBe('59s')
    expect(formatElapsed(90_000, { seconds: 'under-minute' })).toBe('1m')
    expect(formatElapsed(3_725_000, { seconds: 'under-minute' })).toBe('1h 2m')
  })

  it('never shows a negative or invalid time', () => {
    expect(formatElapsed(-5_000)).toBe('0s')
    expect(formatElapsed(Number.NaN)).toBe('0s')
    expect(formatElapsed(Number.POSITIVE_INFINITY)).toBe('0s')
  })
})
