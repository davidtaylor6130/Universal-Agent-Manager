import { describe, expect, it } from 'vitest'
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

it('reserves only Ctrl+Shift+L for focusing the active input', () => {
  expect(matchShortcut(key('L', { ctrlKey: true, shiftKey: true }))).toBe('focusActiveInput')
  expect(matchShortcut(key('l', { ctrlKey: true }))).toBeNull()
  expect(matchShortcut(key('l', { ctrlKey: true, shiftKey: true, altKey: true }))).toBeNull()
  expect(matchShortcut(key('l', { ctrlKey: true, metaKey: true, shiftKey: true }))).toBeNull()
})
