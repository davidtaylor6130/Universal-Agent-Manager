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
