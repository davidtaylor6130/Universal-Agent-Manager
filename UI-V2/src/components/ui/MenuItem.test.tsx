import { act, createElement, createRef } from 'react'
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

describe('MenuItem extensions', () => {
  it('is a menuitemcheckbox with aria-checked when checked is defined', () => {
    const { host, root, button } = render(createElement(MenuItem, { label: 'Show hidden', checked: false, onSelect: vi.fn() }))
    expect(button().getAttribute('role')).toBe('menuitemcheckbox')
    expect(button().getAttribute('aria-checked')).toBe('false')
    expect(button().querySelector('svg')).toBeNull()
    act(() => root.unmount()); host.remove()
  })

  it('renders stacked detail on a second line under the label', () => {
    const { host, root } = render(createElement(MenuItem, { label: 'Claude', detail: 'Opus 5', detailLayout: 'stacked', onSelect: vi.fn() }))
    const text = host.querySelector('.uam-menu-item__text')
    expect(text?.querySelector('.uam-menu-item__label')?.textContent).toBe('Claude')
    expect(text?.querySelector('.uam-menu-item__detail')?.textContent).toBe('Opus 5')
    act(() => root.unmount()); host.remove()
  })

  it('keeps inline detail outside the stacked wrapper by default', () => {
    const { host, root } = render(createElement(MenuItem, { label: 'Claude', detail: 'Opus 5', onSelect: vi.fn() }))
    expect(host.querySelector('.uam-menu-item__text')).toBeNull()
    act(() => root.unmount()); host.remove()
  })

  it('renders a trailing pill before the shortcut', () => {
    const { host, root } = render(createElement(MenuItem, { label: 'Beta', trailing: <span className="pill">New</span>, shortcut: 'F2', onSelect: vi.fn() }))
    expect(host.querySelector('.uam-menu-item__trailing .pill')?.textContent).toBe('New')
    expect(host.querySelector('kbd')?.textContent).toBe('F2')
    act(() => root.unmount()); host.remove()
  })

  it('uses the native disabled attribute when nativeDisabled is set', () => {
    const onSelect = vi.fn()
    const { host, root, button } = render(createElement(MenuItem, { label: 'Delete', disabled: true, nativeDisabled: true, disabledReason: 'Stop first', onSelect }))
    expect(button().hasAttribute('disabled')).toBe(true)
    expect(button().hasAttribute('aria-disabled')).toBe(false)
    button().click()
    expect(onSelect).not.toHaveBeenCalled()
    act(() => root.unmount()); host.remove()
  })

  it('forwards ref, style, mouse-down handler and aria passthrough', () => {
    const ref = createRef<HTMLButtonElement>()
    const onMouseDown = vi.fn()
    const { host, root, button } = render(createElement(MenuItem, { ref, label: 'Open', style: { color: 'red' }, onMouseDown, 'aria-haspopup': 'menu', 'aria-expanded': true, onSelect: vi.fn() }))
    expect(ref.current).toBe(button())
    expect(button().style.color).toBe('red')
    expect(button().getAttribute('aria-haspopup')).toBe('menu')
    expect(button().getAttribute('aria-expanded')).toBe('true')
    button().dispatchEvent(new MouseEvent('mousedown', { bubbles: true }))
    expect(onMouseDown).toHaveBeenCalledTimes(1)
    act(() => root.unmount()); host.remove()
  })

  it('still runs a passed onClick before onSelect, and neither when disabled', () => {
    const order: string[] = []
    const { host, root, button } = render(createElement(MenuItem, { label: 'Go', onClick: () => order.push('click'), onSelect: () => order.push('select') }))
    button().click()
    expect(order).toEqual(['click', 'select'])
    act(() => root.unmount()); host.remove()
    const second = render(createElement(MenuItem, { label: 'Go', disabled: true, onClick: () => order.push('click'), onSelect: () => order.push('select') }))
    second.button().click()
    expect(order).toEqual(['click', 'select'])
    act(() => second.root.unmount()); second.host.remove()
  })
})
