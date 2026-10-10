import { describe, expect, it, vi } from 'vitest'
import { matchShortcut, shortcutLabel } from './shortcuts'

const key = (k: string, mods: Partial<Record<'metaKey' | 'ctrlKey' | 'altKey' | 'shiftKey', boolean>> = {}) =>
  ({ key: k, metaKey: false, ctrlKey: false, altKey: false, shiftKey: false, ...mods })

describe('app shortcuts (non-mac test environment)', () => {
  it('matches Ctrl+key and ignores other modifier combinations', () => {
    expect(matchShortcut(key('n', { ctrlKey: true }))).toBe('newChat')
    expect(matchShortcut(key('K', { ctrlKey: true }))).toBe('search')
    expect(matchShortcut(key(',', { ctrlKey: true }))).toBe('settings')
    expect(matchShortcut(key('b', { ctrlKey: true }))).toBe('sidebar')
    expect(matchShortcut(key('n'))).toBeNull()
    expect(matchShortcut(key('n', { ctrlKey: true, shiftKey: true }))).toBeNull()
    expect(matchShortcut(key('n', { metaKey: true }))).toBeNull()
    expect(matchShortcut(key('x', { ctrlKey: true }))).toBeNull()
  })

  it('labels shortcuts for tooltips', () => {
    expect(shortcutLabel('newChat')).toBe('Ctrl+N')
  })
})

describe('attention shortcut', () => {
  it('requires Ctrl+Shift+J and leaves other combinations alone', () => {
    expect(matchShortcut(key('J', { ctrlKey: true, shiftKey: true }))).toBe('nextAttention')
    expect(matchShortcut(key('j', { ctrlKey: true }))).toBeNull()
    expect(matchShortcut(key('j', { ctrlKey: true, shiftKey: true, altKey: true }))).toBeNull()
    expect(matchShortcut(key('j', { ctrlKey: true, metaKey: true, shiftKey: true }))).toBeNull()
    expect(shortcutLabel('nextAttention')).toBe('Ctrl+Shift+J')
  })
})


it('uses Command+Shift+J on macOS', async () => {
  const platform = Object.getOwnPropertyDescriptor(navigator, 'platform')
  Object.defineProperty(navigator, 'platform', { configurable: true, value: 'MacIntel' })
  vi.resetModules()
  try {
    const shortcuts = await import('./shortcuts')
    expect(shortcuts.matchShortcut(key('J', { metaKey: true, shiftKey: true }))).toBe('nextAttention')
    expect(shortcuts.matchShortcut(key('J', { ctrlKey: true, shiftKey: true }))).toBeNull()
    expect(shortcuts.shortcutLabel('nextAttention')).toBe('⌘⇧J')
  } finally {
    if (platform) Object.defineProperty(navigator, 'platform', platform)
    else delete (navigator as Partial<Navigator>).platform
    vi.resetModules()
  }
})
