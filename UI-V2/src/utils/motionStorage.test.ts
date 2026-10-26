import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { motionAllowed } from '../components/ui/Overlay'
import { applyMotionSpeed, readStoredMotion, writeStoredMotion } from './motionStorage'

const styles = readFileSync(resolve(process.cwd(), 'src/index.css'), 'utf8')
const settleRule = (selector: string) => new RegExp(`${selector}[^{]*\\{[^}]*animation-duration:\\s*\\.01ms !important;[^}]*animation-iteration-count:\\s*1 !important;[^}]*transition-duration:\\s*\\.01ms !important`, 's')

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

  it('Motion Off stops fixed-duration animations through one shared rule, matching OS reduced motion', () => {
    expect(styles).toMatch(settleRule('\\[data-motion="off"\\] \\*'))
    const reduced = styles.slice(styles.indexOf('@media (prefers-reduced-motion: reduce) {\n  *, *::before'))
    expect(reduced).toMatch(settleRule('\\*, \\*::before, \\*::after'))
    expect(styles).toMatch(/@media \(prefers-reduced-motion: reduce\) \{ :root \{ --motion-scale: 0; \} \}/)
  })

  it('stops JS-driven exit motion when Appearance Motion is Off', () => {
    vi.stubGlobal('matchMedia', () => ({ matches: false }))
    expect(motionAllowed()).toBe(true)
    applyMotionSpeed('off')
    expect(document.documentElement.getAttribute('data-motion')).toBe('off')
    expect(motionAllowed()).toBe(false)
  })
})
