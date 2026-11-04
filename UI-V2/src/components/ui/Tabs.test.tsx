import { act, createElement, createRef, useState } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { Tab, Tabs } from './Tabs'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

function Harness({ onChange, variant, disabledB }: { onChange: (value: string) => void; variant?: 'underline' | 'pill'; disabledB?: boolean }) {
  const [value, setValue] = useState('one')
  return createElement(Tabs<string>, {
    value, variant, 'aria-label': 'Scope',
    onChange: (next: string) => { setValue(next); onChange(next) },
    children: [
      createElement(Tab, { key: 'one', value: 'one' }, 'One'),
      createElement(Tab, { key: 'two', value: 'two', disabled: disabledB }, 'Two'),
      createElement(Tab, { key: 'three', value: 'three' }, 'Three'),
    ],
  })
}

function render(node: React.ReactElement) {
  const host = document.createElement('div')
  document.body.appendChild(host)
  const root = createRoot(host)
  act(() => root.render(node))
  return { host, root, tabs: () => Array.from(host.querySelectorAll<HTMLButtonElement>('[role="tab"]')) }
}

describe('Tabs', () => {
  it('renders a labelled tablist with aria-selected and roving tabindex', () => {
    const { host, root, tabs } = render(createElement(Harness, { onChange: vi.fn() }))
    expect(host.querySelector('[role="tablist"]')?.getAttribute('aria-label')).toBe('Scope')
    expect(tabs().map(tab => tab.getAttribute('aria-selected'))).toEqual(['true', 'false', 'false'])
    expect(tabs().map(tab => tab.tabIndex)).toEqual([0, -1, -1])
    act(() => root.unmount()); host.remove()
  })

  it('selects on click and reports the value', () => {
    const onChange = vi.fn()
    const { host, root, tabs } = render(createElement(Harness, { onChange }))
    act(() => tabs()[2].click())
    expect(onChange).toHaveBeenCalledWith('three')
    expect(tabs().map(tab => tab.getAttribute('aria-selected'))).toEqual(['false', 'false', 'true'])
    act(() => root.unmount()); host.remove()
  })

  it('moves focus and selection with arrow keys, wrapping, and Home/End', () => {
    const onChange = vi.fn()
    const { host, root, tabs } = render(createElement(Harness, { onChange }))
    const list = host.querySelector('[role="tablist"]') as HTMLElement
    tabs()[0].focus()
    act(() => { tabs()[0].dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowRight', bubbles: true })) })
    expect(document.activeElement).toBe(tabs()[1])
    expect(onChange).toHaveBeenLastCalledWith('two')
    act(() => { tabs()[1].dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowLeft', bubbles: true })) })
    act(() => { tabs()[0].dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowLeft', bubbles: true })) })
    expect(document.activeElement).toBe(tabs()[2])
    act(() => { tabs()[2].dispatchEvent(new KeyboardEvent('keydown', { key: 'Home', bubbles: true })) })
    expect(document.activeElement).toBe(tabs()[0])
    act(() => { tabs()[0].dispatchEvent(new KeyboardEvent('keydown', { key: 'End', bubbles: true })) })
    expect(document.activeElement).toBe(tabs()[2])
    expect(list).toBeTruthy()
    act(() => root.unmount()); host.remove()
  })

  it('skips disabled tabs when moving with arrow keys', () => {
    const { host, root, tabs } = render(createElement(Harness, { onChange: vi.fn(), disabledB: true }))
    act(() => { tabs()[0].dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowRight', bubbles: true })) })
    expect(document.activeElement).toBe(tabs()[2])
    act(() => root.unmount()); host.remove()
  })

  it('renders the sliding indicator for the selected tab and sets the variant', () => {
    const { host, root } = render(createElement(Harness, { onChange: vi.fn(), variant: 'pill' }))
    expect(host.querySelector('.uam-tabs__indicator')).not.toBeNull()
    expect(host.querySelector('[role="tablist"]')?.getAttribute('data-variant')).toBe('pill')
    act(() => root.unmount()); host.remove()
  })

  it('defaults to the underline variant', () => {
    const { host, root } = render(createElement(Harness, { onChange: vi.fn() }))
    expect(host.querySelector('[role="tablist"]')?.getAttribute('data-variant')).toBe('underline')
    act(() => root.unmount()); host.remove()
  })

  it('forwards ref and button attributes on Tab', () => {
    const ref = createRef<HTMLButtonElement>()
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(createElement(Tabs<string>, { value: 'a', onChange: vi.fn(), 'aria-label': 'X', children: createElement(Tab, { ref, value: 'a', id: 'tab-a', 'aria-controls': 'panel-a' }, 'A') })))
    expect(ref.current).toBe(host.querySelector('#tab-a'))
    expect(ref.current?.getAttribute('aria-controls')).toBe('panel-a')
    act(() => root.unmount()); host.remove()
  })

  it('throws when Tab is used outside Tabs', () => {
    const host = document.createElement('div')
    const root = createRoot(host)
    const spy = vi.spyOn(console, 'error').mockImplementation(() => {})
    expect(() => act(() => root.render(createElement(Tab, { value: 'a' }, 'A')))).toThrow('Tab must be rendered inside Tabs')
    spy.mockRestore()
    act(() => root.unmount())
  })
})
