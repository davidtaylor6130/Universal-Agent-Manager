import { act, createElement } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { RowButton } from './RowButton'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

describe('RowButton', () => {
  it('renders a type=button row, keeps the caller class and marks the selected row', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    const onClick = vi.fn()
    act(() => root.render(createElement(RowButton, { className: 'my-row', selected: true, onClick }, 'src/app.ts')))
    const button = host.querySelector('button') as HTMLButtonElement
    expect(button.type).toBe('button')
    expect(button.className).toContain('uam-row-button')
    expect(button.classList.contains('my-row')).toBe(true)
    expect(button.getAttribute('aria-current')).toBe('true')
    button.click()
    expect(onClick).toHaveBeenCalledTimes(1)
    act(() => root.unmount()); host.remove()
  })

  it('omits aria-current when not selected', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(createElement(RowButton, null, 'Row')))
    expect(host.querySelector('button')?.hasAttribute('aria-current')).toBe(false)
    act(() => root.unmount()); host.remove()
  })
})
