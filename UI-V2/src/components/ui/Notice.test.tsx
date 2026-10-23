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

  it('keeps a nondismissible status visible while details can collapse', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<Notice tone="warning" title="Unconfirmed settings changes" dismissLabel="Dismiss warning" dismissible={false}>Retained draft</Notice>))
      expect(host.querySelector('[role="status"]')).not.toBeNull()
      expect(host.querySelector('button[aria-label="Dismiss warning"]')).toBeNull()
      const header = host.querySelector('button[aria-expanded]') as HTMLButtonElement
      act(() => header.click())
      expect(host.textContent).toContain('Unconfirmed settings changes')
      expect(host.textContent).not.toContain('Retained draft')
      act(() => header.click())
      expect(host.textContent).toContain('Retained draft')
      act(() => root.render(<Notice tone="warning" title="Unconfirmed settings changes" dismissLabel="Dismiss warning" dismissible={false}>Updated draft</Notice>))
      expect(host.textContent).toContain('Updated draft')
      expect(host.querySelector('button[aria-label="Dismiss warning"]')).toBeNull()
    } finally {
      act(() => root.unmount())
      host.remove()
    }
  })
})
