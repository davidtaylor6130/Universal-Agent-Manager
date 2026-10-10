import { act, createRef } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { MessageNavigationRail, messageIndexAtPointer } from './MessageNavigationRail'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

describe('Message navigation rail', () => {
  it('maps equal message positions and clamps pointer coordinates', () => {
    expect(messageIndexAtPointer(101, 20, 200, 120)).toBe(50)
    expect(messageIndexAtPointer(101, 20, 200, -100)).toBe(0)
    expect(messageIndexAtPointer(101, 20, 200, 500)).toBe(100)
    expect(messageIndexAtPointer(1, 0, 0, 10)).toBe(0)
  })

  it('allocates positions only to user messages and keeps the orange marker', () => {
    const host = document.createElement('div')
    document.body.append(host)
    const root = createRoot(host)
    const onNavigate = vi.fn()
    const messages = (['user', 'assistant', 'assistant', 'user', 'assistant'] as const).map((role, index) => ({ id: String(index), sessionId: 'chat', role, content: 'message', createdAt: new Date() }))
    try {
      act(() => root.render(<MessageNavigationRail startIndex={50} messages={messages} currentIndex={54} viewportRef={createRef()} busy={false} onNavigate={onNavigate} />))
      const slider = host.querySelector('[role="slider"]')!
      expect(slider.getAttribute('aria-valuemax')).toBe('2')
      expect(slider.getAttribute('aria-valuenow')).toBe('2')
      expect(host.querySelectorAll('span')[1].style.background).toBe('var(--accent)')
      act(() => slider.dispatchEvent(new KeyboardEvent('keydown', { key: 'Home', bubbles: true })))
      expect(onNavigate).toHaveBeenLastCalledWith(50)
      act(() => slider.dispatchEvent(new KeyboardEvent('keydown', { key: 'End', bubbles: true })))
      expect(onNavigate).toHaveBeenLastCalledWith(53)
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('supports keyboard message jumps and blocks another jump during loading', () => {
    const host = document.createElement('div')
    document.body.append(host)
    const root = createRoot(host)
    const onNavigate = vi.fn()
    const viewportRef = createRef<HTMLDivElement>()
    const render = (busy: boolean) => root.render(<MessageNavigationRail userMessageIndices={[0, 10, 99999]} startIndex={99950} messages={[]}
      currentIndex={99999} viewportRef={viewportRef} busy={busy} onNavigate={onNavigate} />)
    try {
      act(() => render(false))
      const slider = host.querySelector('[role="slider"]')!
      act(() => slider.dispatchEvent(new KeyboardEvent('keydown', { key: 'Home', bubbles: true })))
      expect(onNavigate).toHaveBeenLastCalledWith(0)
      act(() => slider.dispatchEvent(new KeyboardEvent('keydown', { key: 'PageDown', bubbles: true })))
      expect(onNavigate).toHaveBeenLastCalledWith(99999)
      expect(slider.getAttribute('aria-valuenow')).toBe('3')
      expect(host.querySelectorAll('span').length).toBeLessThan(5)
      act(() => render(true))
      onNavigate.mockClear()
      act(() => slider.dispatchEvent(new KeyboardEvent('keydown', { key: 'End', bubbles: true })))
      expect(onNavigate).not.toHaveBeenCalled()
    } finally {
      act(() => root.unmount())
      host.remove()
    }
  })
})
