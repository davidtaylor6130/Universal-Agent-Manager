import { describe, expect, it, vi } from 'vitest'
import { applyDocumentTheme, readStoredTheme, resolveDocumentTheme, writeStoredTheme } from './themeStorage'

describe('themeStorage', () => {
  it('returns null when localStorage reads throw', () => {
    const spy = vi.spyOn(Storage.prototype, 'getItem').mockImplementation(() => {
      throw new Error('blocked')
    })

    expect(readStoredTheme()).toBeNull()
    spy.mockRestore()
  })

  it('ignores localStorage write failures', () => {
    const spy = vi.spyOn(Storage.prototype, 'setItem').mockImplementation(() => {
      throw new Error('blocked')
    })

    expect(() => writeStoredTheme('light')).not.toThrow()
    spy.mockRestore()
  })

  it('applies a valid theme to the document', () => {
    applyDocumentTheme('light')
    expect(document.documentElement.getAttribute('data-theme')).toBe('light')
  })

  it('preserves the system theme preference while applying a resolved document theme', () => {
    const originalMatchMedia = window.matchMedia
    Object.defineProperty(window, 'matchMedia', {
      configurable: true,
      value: vi.fn((query: string) => ({
      matches: query === '(prefers-color-scheme: dark)',
      media: query,
      onchange: null,
      addListener: vi.fn(),
      removeListener: vi.fn(),
      addEventListener: vi.fn(),
      removeEventListener: vi.fn(),
      dispatchEvent: vi.fn(),
      })),
    })

    expect(resolveDocumentTheme('system')).toBe('dark')
    applyDocumentTheme('system')
    expect(document.documentElement.getAttribute('data-theme')).toBe('dark')

    Object.defineProperty(window, 'matchMedia', {
      configurable: true,
      value: originalMatchMedia,
    })
  })
})
