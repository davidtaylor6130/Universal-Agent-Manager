import { act, createElement, createRef } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { Chip } from './Chip'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

function render(node: React.ReactElement) {
  const host = document.createElement('div')
  document.body.appendChild(host)
  const root = createRoot(host)
  act(() => root.render(node))
  return { host, root, button: () => host.querySelector('button') as HTMLButtonElement }
}

describe('Chip', () => {
  it('is a plain type=button with the choice-chip look and no pressed state by default', () => {
    const { host, root, button } = render(createElement(Chip, null, 'Load later'))
    expect(button().type).toBe('button')
    expect(button().classList.contains('uam-choice-button')).toBe(true)
    expect(button().classList.contains('uam-chip--md')).toBe(true)
    expect(button().hasAttribute('aria-pressed')).toBe(false)
    expect(button().textContent).toBe('Load later')
    act(() => root.unmount()); host.remove()
  })

  it('exposes selected as aria-pressed and data-selected', () => {
    const { host, root, button } = render(createElement(Chip, { selected: true }, 'Follow live'))
    expect(button().getAttribute('aria-pressed')).toBe('true')
    expect(button().dataset.selected).toBe('true')
    act(() => root.unmount()); host.remove()
  })

  it('renders the icon before the label and applies tone and size', () => {
    const { host, root, button } = render(createElement(Chip, { icon: createElement('svg', { 'data-icon': 'x' }), tone: 'danger', size: 'sm' }, 'Stop'))
    expect(button().firstElementChild?.getAttribute('data-icon')).toBe('x')
    expect(button().dataset.tone).toBe('danger')
    expect(button().classList.contains('uam-chip--sm')).toBe(true)
    act(() => root.unmount()); host.remove()
  })

  it('passes button attributes, className, style and ref through, and respects disabled', () => {
    const ref = createRef<HTMLButtonElement>()
    const onClick = vi.fn()
    const { host, root, button } = render(createElement(Chip, { ref, className: 'extra', style: { marginTop: 4 }, disabled: true, title: 'Nope', onClick, 'aria-controls': 'panel' }, 'Go'))
    expect(ref.current).toBe(button())
    expect(button().classList.contains('extra')).toBe(true)
    expect(button().style.marginTop).toBe('4px')
    expect(button().disabled).toBe(true)
    expect(button().getAttribute('aria-controls')).toBe('panel')
    button().click()
    expect(onClick).not.toHaveBeenCalled()
    act(() => root.unmount()); host.remove()
  })
})
