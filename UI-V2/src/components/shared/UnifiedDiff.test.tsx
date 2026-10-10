import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it } from 'vitest'
import { parseUnifiedDiff, UnifiedDiff } from './UnifiedDiff'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

describe('Unified file review', () => {
  it('tracks both source ranges across hunks and no-newline markers', () => {
    const rows = parseUnifiedDiff('--- a/file\n+++ b/file\n@@ -8,2 +12,2 @@\n same\n-old\n+new\n\\ No newline at end of file\n@@ -20 +30 @@\n-removed\n+added')
    expect(rows.slice(3, 6)).toEqual([
      { text: ' same', kind: 'context', oldLine: 8, newLine: 12 },
      { text: '-old', kind: 'removed', oldLine: 9 },
      { text: '+new', kind: 'added', newLine: 13 },
    ])
    expect(rows[8]).toMatchObject({ oldLine: 20, kind: 'removed' })
    expect(rows[9]).toMatchObject({ newLine: 30, kind: 'added' })
  })

  it('does not assign source numbers to another file header', () => {
    const rows = parseUnifiedDiff('@@ -1 +1 @@\n+x\ndiff --git a/other b/other\n--- a/other\n+++ b/other')
    expect(rows.slice(2).every((row) => row.oldLine === undefined && row.newLine === undefined)).toBe(true)
  })

  it('bounds DOM rows for a large patch and renders the scrolled range', () => {
    const host = document.createElement('div')
    document.body.append(host)
    const root = createRoot(host)
    try {
      const diff = '@@ -0,0 +1,10000 @@\n' + Array.from({ length: 10000 }, (_, index) => `+line ${index}`).join('\n')
      act(() => root.render(<UnifiedDiff diff={diff} path="large.txt" />))
      expect(host.querySelectorAll('[data-diff-row]').length).toBeLessThan(60)
      const viewport = host.querySelector<HTMLElement>('[role="region"]')!
      Object.defineProperty(viewport, 'clientHeight', { value: 400 })
      viewport.scrollTop = 180000
      act(() => viewport.dispatchEvent(new Event('scroll', { bubbles: true })))
      expect(host.textContent).toContain('line 9000')
      expect(host.querySelectorAll('[data-diff-row]').length).toBeLessThan(60)
      expect(host.querySelector('[data-diff-row="0"]')).toBeNull()
    } finally {
      act(() => root.unmount())
      host.remove()
    }
  })
})
