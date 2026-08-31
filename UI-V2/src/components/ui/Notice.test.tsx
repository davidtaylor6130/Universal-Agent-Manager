import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it } from 'vitest'
import { Notice } from './Notice'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

describe('Notice', () => {
  it('uses a collapsible UAM header and can dismiss itself', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<Notice tone="warning" title="Permission warning" dismissLabel="Dismiss warning">Details</Notice>))

    const header = host.querySelector('button[aria-expanded]') as HTMLButtonElement
    expect(header.getAttribute('aria-expanded')).toBe('true')
    expect(host.textContent).toContain('Details')

    act(() => header.click())
    expect(header.getAttribute('aria-expanded')).toBe('false')
    expect(host.textContent).not.toContain('Details')

    act(() => (host.querySelector('button[aria-label="Dismiss warning"]') as HTMLButtonElement).click())
    expect(host.textContent).toBe('')

    act(() => root.unmount())
    host.remove()
  })
})
