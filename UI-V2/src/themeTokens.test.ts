import { readFileSync } from 'node:fs'
import { describe, expect, it } from 'vitest'

const source = (path: string) => readFileSync(new URL(path, import.meta.url), 'utf8')
const styles = source('./index.css')
const messageBlocks = source('./components/chat/MessageBlocks.tsx')
const chatView = source('./components/views/ChatView.tsx')

describe('theme token contract', () => {
  it('defines every semantic token used by chat feedback', () => {
    expect(styles).toContain('--purple:')
    expect(messageBlocks).not.toContain('var(--danger)')
    expect(chatView).not.toContain('var(--danger)')
  })

  it('lets built-in themes override the default semantic fills', () => {
    expect(styles.indexOf('/* Semantic colors')).toBeLessThan(styles.indexOf('[data-theme="midnight"]'))
  })

  it('defines the restrained Focus default with legible semantic colors and preserves custom themes', () => {
    expect(styles).toContain('[data-theme="focus"]')
    expect(styles).toMatch(/\[data-theme="focus"\]\s*\{[^}]*--accent:\s*#cf7538/s)
    expect(styles).not.toMatch(/\[data-theme="focus"\]\s*\{[^}]*#8b5cf6/s)
    expect(styles).toMatch(/\[data-theme="focus"\]\s*\{[^}]*--green:\s*#4ade80/s)
    expect(styles).toMatch(/\[data-theme="focus"\]\s*\{[^}]*--red:\s*#fb7185/s)
    expect(styles).toMatch(/\[data-theme="focus"\]\s*\{[^}]*--yellow:\s*#fbbf24/s)
    expect(source('./App.tsx')).toContain('applyDocumentTheme(theme, customThemes)')
  })

  it('keeps default text selection visibly distinct from the page surface', () => {
    const selectionRule = styles.match(/::selection\s*\{([^}]*)\}/s)?.[1] ?? ''
    const accentStrength = Number(selectionRule.match(/background:\s*color-mix\(in srgb,\s*var\(--accent\)\s+(\d+)%/)?.[1])

    expect(accentStrength).toBeGreaterThanOrEqual(50)
    expect(selectionRule).toMatch(/color:\s*var\(--text\)/)
  })
})
