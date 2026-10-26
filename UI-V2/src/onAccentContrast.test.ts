import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'
import { describe, expect, it } from 'vitest'
import { applyDocumentTheme, BUILT_IN_THEMES, contrastRatio, onAccentFor } from './utils/themeStorage'

const styles = readFileSync(resolve(process.cwd(), 'src/index.css'), 'utf8')
const AA = 4.5

const hex = (value: string) => value.length === 4 ? `#${[...value.slice(1)].map((c) => c + c).join('')}` : value.toLowerCase()
const block = (selector: string) => {
  const escaped = selector.replace(/[[\]"]/g, (c) => `\\${c}`)
  const blocks = [...styles.matchAll(new RegExp(`(?:^|\\n)${escaped}\\s*\\{([^}]*)\\}`, 'g'))].map((match) => match[1])
  return blocks.find((body) => body.includes('--accent:')) ?? ''
}
const token = (body: string, name: string) => body.match(new RegExp(`${name}:\\s*(#[0-9a-fA-F]{3,6})\\b`))?.[1]
// color-mix(in srgb, a p%, b): per-channel interpolation of the gamma-encoded values.
const mix = (a: string, b: string, share: number) => `#${[1, 3, 5].map((i) => Math.round(parseInt(a.slice(i, i + 2), 16) * share + parseInt(b.slice(i, i + 2), 16) * (1 - share)).toString(16).padStart(2, '0')).join('')}`

describe('on-accent contrast', () => {
  const root = block(':root')
  const themes = BUILT_IN_THEMES.filter((theme) => theme.id !== 'system').map((theme) => {
    const body = theme.id === 'light' ? root : block(`[data-theme="${theme.id}"]`)
    const pick = (name: string) => hex(token(body, name) ?? token(root, name) ?? '')
    return { id: theme.id, accent: pick('--accent'), onAccent: pick('--on-accent'), text: pick('--text') }
  })

  it.each(themes)('$id: text and icons on the accent meet WCAG AA, including primary hover', ({ accent, onAccent, text }) => {
    expect(accent).toMatch(/^#[0-9a-f]{6}$/)
    expect(onAccent).toMatch(/^#[0-9a-f]{6}$/)
    expect(contrastRatio(accent, onAccent)).toBeGreaterThanOrEqual(AA)
    expect(contrastRatio(mix(accent, text, 0.88), onAccent)).toBeGreaterThanOrEqual(AA)
  })

  it('reports the known High Contrast failure value for white on cyan', () => {
    expect(contrastRatio('#00ffff', '#ffffff')).toBeLessThan(1.3)
  })

  it('derives a readable on-accent for any custom theme accent', () => {
    for (let value = 0; value < 0xffffff; value += 0x0f0f0f) {
      const accent = `#${value.toString(16).padStart(6, '0')}`
      expect(contrastRatio(accent, onAccentFor(accent))).toBeGreaterThanOrEqual(AA)
    }
    const colors = { background: '#101010', surface: '#141414', surfaceUp: '#1a1a1a', text: '#f0f0f0', textMuted: '#a0a0a0', accent: '#00ffff', sidebar: '#121212', userMessage: '#202020', assistantMessage: '#141414', success: '#00ff00', warning: '#ffff00', error: '#ff0000' }
    applyDocumentTheme('custom:cyan', [{ version: 1, id: 'custom:cyan', name: 'Cyan', base: 'dark', colors }])
    expect(document.documentElement.style.getPropertyValue('--on-accent')).toBe('#000000')
    applyDocumentTheme('dark')
    expect(document.documentElement.style.getPropertyValue('--on-accent')).toBe('')
  })

  it('uses the on-accent token instead of fixed white or page colours on accent fills', () => {
    expect(styles).toMatch(/\.uam-btn--primary \{\s*background: var\(--accent\);\s*color: var\(--on-accent\);/)
    expect(styles).not.toMatch(/background: var\(--accent\); color: (#fff|white|var\(--bg\))/)
  })
})
