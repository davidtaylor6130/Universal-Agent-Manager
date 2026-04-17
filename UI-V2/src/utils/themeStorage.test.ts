import { describe, expect, it, vi } from 'vitest'
import { applyDocumentTheme, readStoredTheme, writeStoredTheme } from './themeStorage'

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
})
