import { act, createElement } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { MenuItem } from './MenuItem'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

function render(node: React.ReactElement) {
  const host = document.createElement('div')
  document.body.appendChild(host)
  const root = createRoot(host)
  act(() => root.render(node))
  return { host, root, button: () => host.querySelector('button') as HTMLButtonElement }
}

describe('MenuItem', () => {
  it('is a menuitem that selects on click, with shortcut and detail text', () => {
    const onSelect = vi.fn()
    const { host, root, button } = render(createElement(MenuItem, { label: 'Rename', detail: 'Chat title', shortcut: 'F2', onSelect }))
    expect(button().getAttribute('role')).toBe('menuitem')
    expect(host.querySelector('.uam-menu-item__detail')?.textContent).toBe('Chat title')
    expect(host.querySelector('kbd')?.textContent).toBe('F2')
    button().click()
    expect(onSelect).toHaveBeenCalledTimes(1)
    act(() => root.unmount()); host.remove()
  })

  it('becomes a menuitemradio with a check when selected is defined', () => {
    const { host, root, button } = render(createElement(MenuItem, { label: 'Dark', selected: true, onSelect: vi.fn() }))
    expect(button().getAttribute('role')).toBe('menuitemradio')
    expect(button().getAttribute('aria-checked')).toBe('true')
    expect(button().querySelector('svg')).not.toBeNull()
    act(() => root.unmount()); host.remove()
  })

  it('blocks selection when disabled and describes the reason', () => {
    const onSelect = vi.fn()
    const { host, root, button } = render(createElement(MenuItem, { label: 'Delete', disabled: true, disabledReason: 'Stop the run first', onSelect }))
    expect(button().getAttribute('aria-disabled')).toBe('true')
    expect(button().hasAttribute('disabled')).toBe(false)
    button().click()
    expect(onSelect).not.toHaveBeenCalled()
    const describedBy = button().getAttribute('aria-describedby')
    expect(describedBy && document.getElementById(describedBy)?.textContent).toBe('Stop the run first')
    act(() => root.unmount()); host.remove()
  })

  it('marks danger items with the danger class', () => {
    const { host, root, button } = render(createElement(MenuItem, { label: 'Delete', danger: true, onSelect: vi.fn() }))
    expect(button().classList.contains('uam-menu-item--danger')).toBe(true)
    act(() => root.unmount()); host.remove()
  })
})
