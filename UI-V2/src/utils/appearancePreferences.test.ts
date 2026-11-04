import { describe, expect, it } from 'vitest'
import { applyAppearance, normalizeAppearance } from './appearancePreferences'

describe('appearance preferences', () => {
  it('rejects unknown stored values and maps the rest to root data attributes', () => {
    expect(normalizeAppearance({ chatWidth: 'huge', highContrast: 'yes', diffPalette: 'neon' })).toEqual({ chatWidth: 'comfortable', highContrast: false, diffPalette: 'default', modelTokenLimits: true })

    const root = document.createElement('html')
    applyAppearance({ chatWidth: 'wide', highContrast: true, diffPalette: 'colour-blind' }, root)
    expect(root.getAttribute('data-chat-width')).toBe('wide')
    expect(root.getAttribute('data-contrast')).toBe('more')
    expect(root.getAttribute('data-diff-palette')).toBe('colour-blind')

    applyAppearance({ chatWidth: 'comfortable', highContrast: false, diffPalette: 'default' }, root)
    expect(root.hasAttribute('data-chat-width')).toBe(false)
    expect(root.hasAttribute('data-contrast')).toBe(false)
    expect(root.hasAttribute('data-diff-palette')).toBe(false)
  })
})

it('keeps model token limits on unless explicitly turned off', () => {
  expect(normalizeAppearance(null).modelTokenLimits).toBe(true)
  expect(normalizeAppearance({ modelTokenLimits: 'no' }).modelTokenLimits).toBe(true)
  expect(normalizeAppearance({ modelTokenLimits: false }).modelTokenLimits).toBe(false)
})
