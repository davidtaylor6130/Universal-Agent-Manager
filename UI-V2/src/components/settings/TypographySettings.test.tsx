import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { TypographySettings } from './TypographySettings'
import { applyTypography, DEFAULT_TYPOGRAPHY, normalizeTypography, useTypographyStore } from '../../store/typographyStore'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const stored = new Map<string, string>()
const originalStorage = Object.getOwnPropertyDescriptor(window, 'localStorage')
beforeEach(() => {
  stored.clear()
  Object.defineProperty(window, 'localStorage', { configurable: true, value: {
    getItem: (key: string) => stored.get(key) ?? null,
    setItem: (key: string, value: string) => stored.set(key, value),
    removeItem: (key: string) => stored.delete(key),
  } })
})

afterEach(() => {
  useTypographyStore.getState().reset()
  document.documentElement.removeAttribute('style')
  if (originalStorage) Object.defineProperty(window, 'localStorage', originalStorage)
  vi.restoreAllMocks()
})

describe('Typography preferences', () => {
  it('rejects invalid persisted fonts and bounds sizes without accepting nonnumeric data', () => {
    expect(normalizeTypography(null)).toEqual(DEFAULT_TYPOGRAPHY)
    expect(normalizeTypography({ interfaceFont: '__proto__', codeFont: 'url(evil)', interfaceSize: 99, promptSize: '18', codeSize: NaN, terminalSize: -20 }))
      .toMatchObject({ interfaceFont: 'system', codeFont: 'jetbrains', interfaceSize: 20, promptSize: 13, codeSize: 12, terminalSize: 8 })
  })

  it('keeps usable preferences when browser storage is blocked', () => {
    const logged = vi.spyOn(console, 'error').mockImplementation(() => {})
    Object.defineProperty(window, 'localStorage', { configurable: true, get: () => { throw new Error('Storage blocked') } })
    expect(() => useTypographyStore.getState().update({ promptSize: 18 })).not.toThrow()
    expect(useTypographyStore.getState().preferences.promptSize).toBe(18)
    expect(logged).toHaveBeenCalled()
  })

  it('applies separate interface, prompt and code settings and restores default sizes', () => {
    const preferences = normalizeTypography({ ...DEFAULT_TYPOGRAPHY, interfaceSize: 20, promptSize: 18, codeFont: 'consolas', codeSize: 16 })
    applyTypography(preferences)
    const style = document.documentElement.style
    expect(style.getPropertyValue('--fs-base')).toBe('20px')
    expect(style.getPropertyValue('--fs-prompt')).toBe('18px')
    expect(style.getPropertyValue('--font-mono')).toContain('Consolas')
    expect(style.getPropertyValue('--fs-code')).toBe('16px')
    applyTypography(DEFAULT_TYPOGRAPHY)
    expect(style.getPropertyValue('--fs-base')).toBe('13px')
  })

  it('edits, persists and resets preferences through the settings controls', () => {
    const host = document.createElement('div')
    document.body.append(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<TypographySettings />))
      const font = host.querySelector<HTMLSelectElement>('[aria-label="Terminal font"]')!
      act(() => { font.value = 'consolas'; font.dispatchEvent(new Event('change', { bubbles: true })) })
      const input = host.querySelector<HTMLInputElement>('[aria-label="Message input font size"]')!
      act(() => {
        Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(input, '18')
        input.dispatchEvent(new Event('input', { bubbles: true }))
      })
      expect(useTypographyStore.getState().preferences).toMatchObject({ terminalFont: 'consolas', promptSize: 18 })
      expect(JSON.parse(window.localStorage.getItem('uam-typography-v1')!).state.preferences).toMatchObject({ terminalFont: 'consolas', promptSize: 18 })
      act(() => Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Reset typography')!.click())
      expect(useTypographyStore.getState().preferences).toEqual(DEFAULT_TYPOGRAPHY)
      expect(font.value).toBe('jetbrains')
    } finally {
      act(() => root.unmount())
      host.remove()
    }
  })
})
