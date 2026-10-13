import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { applyMotionSpeed, readStoredMotion, writeStoredMotion } from './motionStorage'

describe('motionStorage', () => {
  beforeEach(() => {
    const items = new Map<string, string>()
    vi.stubGlobal('localStorage', {
      getItem: (key: string) => items.get(key) ?? null,
      setItem: (key: string, value: string) => { items.set(key, value) },
    })
  })

  afterEach(() => {
    vi.unstubAllGlobals()
    document.documentElement.removeAttribute('data-motion')
  })

  it('persists a speed, falls back to default for unknown values, and sets the root attribute', () => {
    expect(readStoredMotion()).toBe('default')

    writeStoredMotion('slow')
    expect(readStoredMotion()).toBe('slow')
    applyMotionSpeed('slow')
    expect(document.documentElement.getAttribute('data-motion')).toBe('slow')

    localStorage.setItem('uam-motion', 'warp')
    expect(readStoredMotion()).toBe('default')

    applyMotionSpeed('default')
    expect(document.documentElement.hasAttribute('data-motion')).toBe(false)
  })
})
