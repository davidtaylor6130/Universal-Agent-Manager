import { describe, expect, it } from 'vitest'

// CI runs the frontend tests with LC_ALL=en_GB.UTF-8, so these fail there if the pin breaks.
describe('test locale pin', () => {
  it('formats with en-US by default whatever the machine locale', () => {
    expect(new Intl.NumberFormat(undefined, { notation: 'compact' }).format(9_800_000)).toBe('9.8M')
    expect(Intl.NumberFormat(undefined, { style: 'currency', currency: 'USD' }).format(3.6)).toBe('$3.60')
    expect((1800).toLocaleString(undefined, { notation: 'compact' })).toBe('1.8K')
    expect(new Date(Date.UTC(2026, 0, 2)).toLocaleDateString(undefined, { timeZone: 'UTC' })).toBe('1/2/2026')
    expect(new Intl.NumberFormat().resolvedOptions().locale).toBe('en-US')
  })

  it('still honours an explicit locale', () => {
    expect(new Intl.NumberFormat('de-DE').format(1234.5)).toBe('1.234,5')
    expect((1234.5).toLocaleString('de-DE')).toBe('1.234,5')
  })
})
