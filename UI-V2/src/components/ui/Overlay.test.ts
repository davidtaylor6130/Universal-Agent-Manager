import { afterEach, describe, expect, it, vi } from 'vitest'
import { scaledMs } from './Overlay'

afterEach(() => vi.unstubAllGlobals())

const stubScale = (value: string) => vi.stubGlobal('getComputedStyle', () => ({ getPropertyValue: () => value }))

describe('scaledMs', () => {
  it('follows Appearance > Animation speed so exits are not cut short', () => {
    stubScale(' 1.5')
    expect(scaledMs(170)).toBe(255)
    stubScale('0.67')
    expect(scaledMs(100)).toBeCloseTo(67)
  })

  it('keeps the base length when no scale is set (Default)', () => {
    stubScale('')
    expect(scaledMs(170)).toBe(170)
  })
})
