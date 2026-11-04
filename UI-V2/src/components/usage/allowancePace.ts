import type { AcpRateLimitWindow } from '../../store/cpp/types'

/**
 * How a window's use compares with the time that has passed in it.
 * out: used up. ahead: a straight-line burn runs out before the reset. on: within the margin of time elapsed.
 * under: well below the time elapsed. unknown: no window length, no future reset, or no time elapsed yet.
 */
export type PaceVerdict = 'out' | 'ahead' | 'on' | 'under' | 'unknown'

export interface AllowancePace {
  verdict: PaceVerdict
  /** Share of the window's time already passed (0-100); null when the window length or a future reset is unknown. */
  elapsedPercent: number | null
  /** When a straight-line burn reaches 100%, in ms; only set for "ahead", and always before the reset. */
  runsOutAt: number | null
}

/** Percentage points of slack either side of the elapsed share before use counts as ahead of, or well under, pace. */
export const PACE_MARGIN = 5

/**
 * Projects a window linearly from its start: use so far divided by time so far, carried to 100%.
 * The window starts at resetsAt minus its reported length; nothing is projected without both.
 */
export function allowancePace(window: Pick<AcpRateLimitWindow, 'usedPercent' | 'resetsAt' | 'windowDurationMinutes'>, now: number): AllowancePace {
  const used = window.usedPercent
  const unknown: AllowancePace = { verdict: 'unknown', elapsedPercent: null, runsOutAt: null }
  if (!Number.isFinite(used) || used < 0) return unknown
  if (used >= 100) return { ...unknown, verdict: 'out' }
  const reset = window.resetsAt === null ? NaN : window.resetsAt * 1000
  const length = (window.windowDurationMinutes ?? 0) * 60000
  if (!Number.isFinite(reset) || !(length > 0) || reset <= now) return unknown
  const start = reset - length
  const elapsed = Math.min(length, Math.max(0, now - start))
  const elapsedPercent = elapsed / length * 100
  if (elapsed === 0) return { ...unknown, elapsedPercent }
  if (used - elapsedPercent >= PACE_MARGIN) return { verdict: 'ahead', elapsedPercent, runsOutAt: start + elapsed * 100 / used }
  return { verdict: elapsedPercent - used >= PACE_MARGIN ? 'under' : 'on', elapsedPercent, runsOutAt: null }
}
