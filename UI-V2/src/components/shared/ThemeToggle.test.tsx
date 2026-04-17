import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it } from 'vitest'
import { ThemeToggle } from './ThemeToggle'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

describe('ThemeToggle', () => {
  it('renders in the DOM test environment', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ThemeToggle />)
    })

    expect(host.querySelector('button')?.title).toBe('Switch to light mode')

    act(() => {
      root.unmount()
    })
    host.remove()
  })
})
