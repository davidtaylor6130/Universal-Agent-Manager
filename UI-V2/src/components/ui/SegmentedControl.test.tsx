import { act, createElement } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { SegmentedControl } from './SegmentedControl'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

describe('SegmentedControl', () => {
  it('labels the group, marks the pressed option and reports the clicked value', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    const onChange = vi.fn()
    act(() => root.render(createElement(SegmentedControl, { 'aria-label': 'Usage period', value: 7, onChange, options: [{ value: 7, label: '7d' }, { value: 30, label: '30d' }] })))
    expect(host.querySelector('[role="group"]')?.getAttribute('aria-label')).toBe('Usage period')
    const buttons = Array.from(host.querySelectorAll('button'))
    expect(buttons.map(button => button.getAttribute('aria-pressed'))).toEqual(['true', 'false'])
    buttons[1].click()
    expect(onChange).toHaveBeenCalledWith(30)
    expect(host.querySelector('.uam-seg__indicator')).not.toBeNull()
    act(() => root.unmount()); host.remove()
  })
})

describe('SegmentedControl option extensions', () => {
  it('disables an individual option without disabling the group', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    const onChange = vi.fn()
    act(() => root.render(createElement(SegmentedControl, { 'aria-label': 'Scope', value: 'a', onChange, options: [{ value: 'a', label: 'A' }, { value: 'b', label: 'B', disabled: true }] })))
    const buttons = Array.from(host.querySelectorAll('button'))
    expect(buttons[1].disabled).toBe(true)
    expect(buttons[0].disabled).toBe(false)
    act(() => root.unmount()); host.remove()
  })

  it('icon-only options use the label as accessible name and hide the text', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(createElement(SegmentedControl, { 'aria-label': 'View', value: 'list', onChange: vi.fn(), options: [{ value: 'list', label: 'List view', icon: createElement('svg'), iconOnly: true }] })))
    const button = host.querySelector('button') as HTMLButtonElement
    expect(button.getAttribute('aria-label')).toBe('List view')
    expect(button.querySelector('.uam-btn__label')).toBeNull()
    expect(button.classList.contains('uam-seg__icon')).toBe(true)
    act(() => root.unmount()); host.remove()
  })

  it('ariaLabel overrides the accessible name of a labelled option', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(createElement(SegmentedControl, { 'aria-label': 'Period', value: 7, onChange: vi.fn(), options: [{ value: 7, label: '7d', ariaLabel: 'Last 7 days' }] })))
    expect(host.querySelector('button')?.getAttribute('aria-label')).toBe('Last 7 days')
    expect(host.querySelector('button')?.textContent).toBe('7d')
    act(() => root.unmount()); host.remove()
  })
})
