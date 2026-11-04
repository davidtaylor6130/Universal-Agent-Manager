import { act, createElement, createRef } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { IconButton } from './IconButton'
import { Tooltip } from './Tooltip'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

function render(node: React.ReactElement) {
  const host = document.createElement('div')
  document.body.appendChild(host)
  const root = createRoot(host)
  act(() => root.render(node))
  return { host, root, button: () => host.querySelector('button') as HTMLButtonElement }
}

describe('IconButton extensions', () => {
  it('owns its tooltip trigger by default', () => {
    const { host, root, button } = render(createElement(IconButton, { icon: createElement('svg'), label: 'Close' }))
    expect(button().hasAttribute('data-state')).toBe(true)
    act(() => root.unmount()); host.remove()
  })

  it('skips its own tooltip when tooltip={false} so a caller can wrap it', () => {
    const { host, root, button } = render(createElement(Tooltip, { label: 'Outer' }, createElement(IconButton, { icon: createElement('svg'), label: 'Close', tooltip: false })))
    expect(button().hasAttribute('data-state')).toBe(true)
    expect(button().getAttribute('aria-label')).toBe('Close')
    act(() => root.unmount()); host.remove()
  })

  it('passes role, aria-checked and style through for toggles, and forwards ref', () => {
    const ref = createRef<HTMLButtonElement>()
    const { host, root, button } = render(createElement(IconButton, { ref, icon: createElement('svg'), label: 'Pin', tooltip: false, role: 'checkbox', 'aria-checked': true, style: { opacity: 0.5 } }))
    expect(ref.current).toBe(button())
    expect(button().getAttribute('role')).toBe('checkbox')
    expect(button().getAttribute('aria-checked')).toBe('true')
    expect(button().style.opacity).toBe('0.5')
    act(() => root.unmount()); host.remove()
  })

  it('still calls onClick when tooltip is off', () => {
    const onClick = vi.fn()
    const { host, root, button } = render(createElement(IconButton, { icon: createElement('svg'), label: 'Go', tooltip: false, onClick }))
    button().click()
    expect(onClick).toHaveBeenCalledTimes(1)
    act(() => root.unmount()); host.remove()
  })
})
