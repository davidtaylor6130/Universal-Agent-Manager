import { act, createElement } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { MenuSelect } from './MenuSelect'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

describe('MenuSelect id', () => {
  it('applies the id to the trigger so a label htmlFor can reach it', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      act(() => root.render(createElement(MenuSelect, { id: 'picker', label: 'Provider', value: 'a', options: [{ value: 'a', label: 'A' }], onChange: vi.fn() })))
      expect(host.querySelector('button#picker')?.getAttribute('role')).toBe('combobox')
    } finally { act(() => root.unmount()); host.remove() }
  })
})

describe('MenuSelect disabled options', () => {
  it('rejects hidden options and skips them during keyboard navigation', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    const onChange = vi.fn()
    const options = [
      { value: 'hidden', label: 'Hidden model', disabled: true },
      { value: 'available', label: 'Available model' },
      { value: 'other-hidden', label: 'Other hidden model', disabled: true },
      { value: 'other', label: 'Other model' },
    ]
    try {
      act(() => root.render(createElement(MenuSelect, { label: 'Worker model', value: 'hidden', options, onChange })))
      act(() => host.querySelector<HTMLButtonElement>('[role="combobox"]')!.click())
      const menu = document.querySelector<HTMLElement>('[role="listbox"]')!
      const buttons = Array.from(menu.querySelectorAll<HTMLButtonElement>('[role="option"]'))
      expect(buttons[0].disabled).toBe(true)
      act(() => buttons[0].click())
      expect(onChange).not.toHaveBeenCalled()
      expect(document.activeElement).toBe(buttons[1])
      act(() => menu.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })))
      expect(document.activeElement).toBe(buttons[3])
      act(() => menu.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })))
      expect(document.activeElement).toBe(buttons[1])
      act(() => menu.dispatchEvent(new KeyboardEvent('keydown', { key: 'End', bubbles: true })))
      expect(document.activeElement).toBe(buttons[3])
      act(() => menu.dispatchEvent(new KeyboardEvent('keydown', { key: 'Home', bubbles: true })))
      expect(document.activeElement).toBe(buttons[1])
      act(() => menu.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowUp', bubbles: true })))
      expect(document.activeElement).toBe(buttons[3])
      act(() => buttons[1].click())
      expect(onChange).toHaveBeenCalledExactlyOnceWith('available')
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('keeps an all-disabled or empty menu inert', () => {
    for (const options of [[], [{ value: 'hidden', label: 'Hidden model', disabled: true }]]) {
      const host = document.createElement('div')
      document.body.appendChild(host)
      const root = createRoot(host)
      const onChange = vi.fn()
      try {
        act(() => root.render(createElement(MenuSelect, { label: 'Reviewer model', value: 'hidden', options, onChange })))
        const trigger = host.querySelector<HTMLButtonElement>('[role="combobox"]')!
        act(() => trigger.click())
        const menu = document.querySelector<HTMLElement>('[role="listbox"]')!
        act(() => menu.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })))
        act(() => menu.dispatchEvent(new KeyboardEvent('keydown', { key: 'Home', bubbles: true })))
        for (const button of menu.querySelectorAll<HTMLButtonElement>('[role="option"]')) act(() => button.click())
        expect(onChange).not.toHaveBeenCalled()
        expect(document.activeElement).toBe(trigger)
        act(() => menu.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
        expect(trigger.getAttribute('aria-expanded')).toBe('false')
      } finally { act(() => root.unmount()); host.remove() }
    }
  })

  it('redirects focus when a live model list disables its focused option', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    const onChange = vi.fn()
    const render = (disabled: boolean) => act(() => root.render(createElement(MenuSelect, {
      label: 'Worker model', value: 'first', onChange,
      options: [{ value: 'first', label: 'First', disabled }, { value: 'second', label: 'Second' }],
    })))
    try {
      render(false)
      act(() => host.querySelector<HTMLButtonElement>('[role="combobox"]')!.click())
      const buttons = Array.from(document.querySelectorAll<HTMLButtonElement>('[role="option"]'))
      expect(document.activeElement).toBe(buttons[0])
      render(true)
      expect(buttons[0].disabled).toBe(true)
      expect(document.activeElement).toBe(buttons[1])
      act(() => buttons[0].click())
      expect(onChange).not.toHaveBeenCalled()
      act(() => buttons[1].click())
      expect(onChange).toHaveBeenCalledExactlyOnceWith('second')
    } finally { act(() => root.unmount()); host.remove() }
  })

})
