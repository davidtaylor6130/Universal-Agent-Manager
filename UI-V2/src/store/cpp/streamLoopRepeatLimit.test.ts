import { describe, expect, it } from 'vitest'
import { normalizeStreamLoopRepeatLimit } from './sanitizers'

describe('normalizeStreamLoopRepeatLimit', () => {
  it('rounds every value to an offered choice exactly as the native normalizer does', () => {
    expect([-1, 0, 2, 10, 11, 20, 21, 500].map(normalizeStreamLoopRepeatLimit)).toEqual([0, 0, 10, 10, 20, 20, 50, 50])
    expect(normalizeStreamLoopRepeatLimit('20')).toBe(20)
  })
})
