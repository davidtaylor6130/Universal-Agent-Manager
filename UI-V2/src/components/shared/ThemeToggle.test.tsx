import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it } from 'vitest'
import { ThemeToggle } from './ThemeToggle'
import { useAppStore } from '../../store/useAppStore'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

describe('ThemeToggle', () => {
  it('renders in the DOM test environment', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ThemeToggle />)
    })

    expect(host.querySelector('button')?.getAttribute('aria-label')).toBe('Switch to light mode')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('treats a light custom theme as light', () => {
    useAppStore.setState({
      theme: 'custom:paper-copy',
      customThemes: [{
        version: 1,
        id: 'custom:paper-copy',
        name: 'Paper Copy',
        base: 'light',
        colors: {
          background: '#ffffff',
          surface: '#ffffff',
          surfaceUp: '#f5f5f5',
          text: '#111111',
          textMuted: '#666666',
          accent: '#ff6600',
          sidebar: '#fafafa',
          userMessage: '#fff0e8',
          assistantMessage: '#ffffff',
          success: '#16a34a',
          warning: '#eab308',
          error: '#ef4444',
        },
      }],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => root.render(<ThemeToggle />))

    expect(host.querySelector('button')?.getAttribute('aria-label')).toBe('Switch to dark mode')

    act(() => root.unmount())
    host.remove()
  })
})
