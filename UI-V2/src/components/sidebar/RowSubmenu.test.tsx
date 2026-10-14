import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { RowSubmenu } from './RowSubmenu'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

beforeEach(() => { vi.useFakeTimers() })
afterEach(() => { vi.useRealTimers() })

function render() {
  const host = document.createElement('div')
  document.body.appendChild(host)
  const root = createRoot(host)
  act(() => root.render(<RowSubmenu label="Copy" ariaLabel="Copy" icon={<span />}><button type="button" role="menuitem">Chat ID</button></RowSubmenu>))
  const trigger = host.querySelector<HTMLButtonElement>('button')!
  const submenu = () => document.body.querySelector<HTMLElement>('[role="menu"][aria-label="Copy"]')
  return { host, root, trigger, submenu, unmount: () => { act(() => root.unmount()); host.remove() } }
}

describe('RowSubmenu', () => {
  it('waits for hover intent before opening and keeps it open while the pointer crosses into it', () => {
    const { trigger, submenu, unmount } = render()
    try {
      act(() => { trigger.dispatchEvent(new MouseEvent('mouseover', { bubbles: true })); vi.advanceTimersByTime(60) })
      expect(submenu()).toBeNull()
      act(() => { vi.advanceTimersByTime(100) })
      expect(submenu()).toBeTruthy()
      expect(trigger.getAttribute('aria-expanded')).toBe('true')
      act(() => { trigger.dispatchEvent(new MouseEvent('mouseout', { bubbles: true, relatedTarget: submenu() })); vi.advanceTimersByTime(60) })
      expect(submenu()?.getAttribute('data-state')).toBe('open')
    } finally { unmount() }
  })

  it('opens from ArrowRight and closes from ArrowLeft with motion before unmounting', () => {
    const { trigger, submenu, unmount } = render()
    try {
      act(() => trigger.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowRight', bubbles: true })))
      expect(submenu()).toBeTruthy()
      expect(submenu()!.contains(document.activeElement)).toBe(true)
      act(() => submenu()!.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowLeft', bubbles: true })))
      expect(submenu()?.getAttribute('data-state')).toBe('closing')
      expect(document.activeElement).toBe(trigger)
      act(() => { vi.advanceTimersByTime(200) })
      expect(submenu()).toBeNull()
    } finally { unmount() }
  })
})
