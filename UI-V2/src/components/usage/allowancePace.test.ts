import { describe, expect, it } from 'vitest'
import { allowancePace, PACE_MARGIN } from './allowancePace'

const HOUR = 3600000
// A 10-hour window resetting at t = 10h, so the window starts at t = 0.
const pace = (usedPercent: number, now: number) => allowancePace({ usedPercent, resetsAt: 10 * HOUR / 1000, windowDurationMinutes: 600 }, now)

describe('allowancePace', () => {
  it('counts use exactly the margin above elapsed as ahead, just below as on', () => {
    expect(pace(50 + PACE_MARGIN, 5 * HOUR).verdict).toBe('ahead')
    expect(pace(50 + PACE_MARGIN - 0.01, 5 * HOUR).verdict).toBe('on')
    expect(pace(50 - PACE_MARGIN, 5 * HOUR).verdict).toBe('under')
  })
  it('projects nothing before any time has elapsed', () => {
    expect(pace(10, 0)).toEqual({ verdict: 'unknown', elapsedPercent: 0, runsOutAt: null })
  })
  it('is unknown once the reset has passed', () => {
    expect(pace(10, 10 * HOUR).verdict).toBe('unknown')
    expect(pace(10, 11 * HOUR).elapsedPercent).toBeNull()
  })
  it('is out at 100% used', () => {
    expect(pace(100, 5 * HOUR).verdict).toBe('out')
  })
  it('runs out before the reset when ahead', () => {
    const result = pace(80, 4 * HOUR)
    expect(result.verdict).toBe('ahead')
    expect(result.runsOutAt).toBe(5 * HOUR)
    expect(result.runsOutAt!).toBeLessThan(10 * HOUR)
  })
})
