import { describe, expect, it, vi } from 'vitest'
import {
  applyDocumentTheme,
  BUILT_IN_THEMES,
  normalizeCustomTheme,
  normalizeStoredTheme,
  readStoredTheme,
  resolveDocumentTheme,
  writeStoredTheme,
} from './themeStorage'

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

  it('makes the built-in palettes available, applies them, and persists the selection', () => {
    const palettes = ['focus', 'midnight', 'paper', 'dusk', 'aurora', 'contrast'] as const
    const values = new Map<string, string>()
    const previousStorage = Object.getOwnPropertyDescriptor(globalThis, 'localStorage')
    Object.defineProperty(globalThis, 'localStorage', {
      configurable: true,
      value: { getItem: (key: string) => values.get(key) ?? null, setItem: (key: string, value: string) => values.set(key, value) },
    })
    expect(BUILT_IN_THEMES.map((theme) => theme.id)).toEqual(expect.arrayContaining([...palettes]))
    expect(BUILT_IN_THEMES.find((theme) => theme.id === 'focus')?.label).toBe('Focus')
    expect(BUILT_IN_THEMES.find((theme) => theme.id === 'dark')?.label).toBe('OG')
    expect(normalizeStoredTheme('mono')).toBe('focus')

    for (const palette of palettes) {
      applyDocumentTheme(palette)
      expect(document.documentElement.getAttribute('data-theme')).toBe(palette)
      writeStoredTheme(palette)
      expect(readStoredTheme()).toBe(palette)
    }
    if (previousStorage) Object.defineProperty(globalThis, 'localStorage', previousStorage)
    else delete (globalThis as { localStorage?: Storage }).localStorage
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

    vi.mocked(window.matchMedia).mockReturnValue({ matches: false } as MediaQueryList)
    applyDocumentTheme('system')
    expect(document.documentElement.getAttribute('data-theme')).toBe('light')

    Object.defineProperty(window, 'matchMedia', {
      configurable: true,
      value: originalMatchMedia,
    })
  })

  it.each(['focus', 'light', 'midnight'] as const)('does not mutate the document when reapplying %s', (theme) => {
    applyDocumentTheme(theme)
    const attribute = vi.spyOn(document.documentElement, 'setAttribute')
    const set = vi.spyOn(document.documentElement.style, 'setProperty')
    const remove = vi.spyOn(document.documentElement.style, 'removeProperty')
    try {
      const started = performance.now()
      for (let index = 0; index < 200; index++) applyDocumentTheme(theme)
      expect(performance.now() - started).toBeLessThan(100)
      expect(attribute).not.toHaveBeenCalled()
      expect(set).not.toHaveBeenCalled()
      expect(remove).not.toHaveBeenCalled()
    } finally {
      attribute.mockRestore()
      set.mockRestore()
      remove.mockRestore()
    }
  })

  it('validates and applies every custom theme color', () => {
    const theme = normalizeCustomTheme({
      version: 1,
      id: 'custom:ocean',
      name: 'Ocean',
      base: 'dark',
      colors: {
        background: '#101820', surface: '#17212B', surfaceUp: '#22303c', text: '#f1f5f9',
        textMuted: '#94a3b8', accent: '#38bdf8', sidebar: '#0b1219', userMessage: '#173047',
        assistantMessage: '#17212b', success: '#22c55e', warning: '#f59e0b', error: '#ef4444',
      },
    })

    expect(theme?.colors.surface).toBe('#17212b')
    applyDocumentTheme('custom:ocean', theme ? [theme] : [])
    expect(document.documentElement.getAttribute('data-theme')).toBe('dark')
    expect(document.documentElement.style.getPropertyValue('--bg')).toBe('#101820')
    expect(document.documentElement.style.getPropertyValue('--message-user-bg')).toBe('#173047')
    expect(document.documentElement.style.getPropertyValue('--accent-dim')).toContain('#38bdf8')
    expect(document.documentElement.style.getPropertyValue('--text-2')).toContain('60%')
    expect(document.documentElement.style.getPropertyValue('--text-3')).toContain('90%')
    if (!theme) throw new Error('Expected valid custom theme')
    const attribute = vi.spyOn(document.documentElement, 'setAttribute')
    const set = vi.spyOn(document.documentElement.style, 'setProperty')
    const remove = vi.spyOn(document.documentElement.style, 'removeProperty')
    try {
      const started = performance.now()
      for (let index = 0; index < 200; index++) applyDocumentTheme(theme.id, [theme])
      expect(performance.now() - started).toBeLessThan(100)
      expect(attribute).not.toHaveBeenCalled()
      expect(set).not.toHaveBeenCalled()
      expect(remove).not.toHaveBeenCalled()

      const updated = { ...theme, colors: { ...theme.colors, accent: '#abcdef' } }
      applyDocumentTheme(theme.id, [updated])
      expect(document.documentElement.style.getPropertyValue('--accent')).toBe('#abcdef')
      expect(document.documentElement.style.getPropertyValue('--accent-dim')).toContain('#abcdef')
      expect(document.documentElement.style.getPropertyValue('--term-cursor')).toBe('#abcdef')
      document.documentElement.style.setProperty('--accent', '#000000')
      applyDocumentTheme(theme.id, [updated])
      expect(document.documentElement.style.getPropertyValue('--accent')).toBe('#abcdef')

      document.documentElement.style.setProperty('--unrelated', 'keep')
      applyDocumentTheme('focus', [updated])
      expect(document.documentElement.getAttribute('data-theme')).toBe('focus')
      expect(document.documentElement.style.length).toBe(1)
      expect(document.documentElement.style.getPropertyValue('--unrelated')).toBe('keep')
      document.documentElement.style.removeProperty('--unrelated')
    } finally {
      attribute.mockRestore()
      set.mockRestore()
      remove.mockRestore()
    }
    expect(normalizeCustomTheme({ ...theme, colors: { ...theme.colors, accent: 'blue' } })).toBeNull()
  })
})
