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

  it('explains unloaded messages in the hover preview', () => {
    const host = document.createElement('div')
    document.body.append(host)
    const root = createRoot(host)
    const messages = [{ id: '0', sessionId: 'chat', role: 'user' as const, content: 'Loaded question', createdAt: new Date() }]
    try {
      act(() => root.render(<MessageNavigationRail userMessageIndices={[0, 2, 40]} startIndex={0} messages={messages} currentIndex={0} viewportRef={createRef()} busy={false} onNavigate={vi.fn()} />))
      const slider = host.querySelector('[role="slider"]')!
      act(() => slider.dispatchEvent(new KeyboardEvent('keydown', { key: 'End', bubbles: true })))
      expect(host.textContent).toContain('Not loaded yet')
      expect(host.textContent).toContain('Scroll up or click to load this part of the chat.')
      expect(host.textContent).not.toContain('Open this message')
      expect(slider.getAttribute('aria-valuetext')).toBe('User message 3 of 3. Not loaded yet. Scroll up or click to load this part of the chat.')
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('shows the preview on focus, exposes its text through the slider value and clears on blur', () => {
    const host = document.createElement('div')
    document.body.append(host)
    const root = createRoot(host)
    const messages = [{ id: '0', sessionId: 'chat', role: 'user' as const, content: 'First\n  question', createdAt: new Date() }]
    try {
      act(() => root.render(<MessageNavigationRail userMessageIndices={[0]} startIndex={0} messages={messages} currentIndex={0} viewportRef={createRef()} busy={false} onNavigate={vi.fn()} />))
      const slider = host.querySelector('[role="slider"]')!
      expect(host.querySelector('.conversation-rail-preview')).toBeNull()
      act(() => slider.dispatchEvent(new FocusEvent('focusin', { bubbles: true })))
      const preview = host.querySelector('.conversation-rail-preview')!
      expect(preview.getAttribute('aria-hidden')).toBe('true')
      expect(preview.textContent).toContain('First')
      expect(slider.getAttribute('aria-valuetext')).toBe('User message 1 of 1: First question')
      act(() => slider.dispatchEvent(new FocusEvent('focusout', { bubbles: true })))
      expect(host.querySelector('.conversation-rail-preview')).toBeNull()
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('keeps the focused preview when the pointer leaves and clears it on blur', () => {
    const host = document.createElement('div')
    document.body.append(host)
    const root = createRoot(host)
    const messages = [{ id: '0', sessionId: 'chat', role: 'user' as const, content: 'Focused question', createdAt: new Date() }]
    try {
      act(() => root.render(<MessageNavigationRail userMessageIndices={[0]} startIndex={0} messages={messages} currentIndex={0} viewportRef={createRef()} busy={false} onNavigate={vi.fn()} />))
      const slider = host.querySelector<HTMLElement>('[role="slider"]')!
      act(() => slider.focus())
      act(() => slider.dispatchEvent(new FocusEvent('focusin', { bubbles: true })))
      act(() => slider.dispatchEvent(new MouseEvent('mouseout', { bubbles: true })))
      expect(document.activeElement).toBe(slider)
      expect(host.querySelector('.conversation-rail-preview')).not.toBeNull()
      act(() => slider.dispatchEvent(new FocusEvent('focusout', { bubbles: true })))
      expect(host.querySelector('.conversation-rail-preview')).toBeNull()
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('insets the rail so expanded marks end within the 64px gutter', () => {
    const host = document.createElement('div')
    document.body.append(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<MessageNavigationRail userMessageIndices={[0, 1, 2]} startIndex={0} messages={[]} currentIndex={0} viewportRef={createRef()} busy={false} onNavigate={vi.fn()} />))
      const rail = host.firstElementChild as HTMLElement
      expect(rail.classList.contains('left-1')).toBe(true) // 4px
      expect(rail.classList.contains('left-0')).toBe(false)
      act(() => host.querySelector('[role="slider"]')!.dispatchEvent(new KeyboardEvent('keydown', { key: 'End', bubbles: true })))
      const mark = host.querySelector<HTMLElement>('[data-rail-neighbor]')!
      expect(4 + parseFloat(mark.style.left) + parseFloat(mark.style.width)).toBeLessThanOrEqual(58)
    } finally { act(() => root.unmount()); host.remove() }
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

  it('expands nearby hover marks while only the click target is orange', () => {
    const host = document.createElement('div')
    document.body.append(host)
    const root = createRoot(host)
    const onNavigate = vi.fn()
    try {
      act(() => root.render(<MessageNavigationRail userMessageIndices={[0, 5, 10, 15, 20, 25, 30]} startIndex={0} messages={[]} currentIndex={0} viewportRef={createRef()} busy={false} onNavigate={onNavigate} />))
      const slider = host.querySelector<HTMLElement>('[role="slider"]')!
      expect(slider.style.height).toBe('48px')
      expect(slider.style.maxHeight).toBe('100%')
      vi.spyOn(slider, 'getBoundingClientRect').mockReturnValue({ top: 20, height: 300 } as DOMRect)
      act(() => slider.dispatchEvent(new MouseEvent('mousemove', { bubbles: true, clientY: 170 })))
      expect(slider.getAttribute('aria-valuenow')).toBe('4')
      expect(host.querySelectorAll('[data-rail-neighbor]')).toHaveLength(4)
      expect(host.querySelector<HTMLElement>('[data-rail-target]')!.style.transform).toBe('scaleX(1)')
      expect(Array.from(host.querySelectorAll<HTMLElement>('span')).filter(mark => mark.style.background === 'var(--accent)')).toHaveLength(1)
      act(() => slider.dispatchEvent(new MouseEvent('click', { bubbles: true, clientY: 170 })))
      expect(onNavigate).toHaveBeenLastCalledWith(15)
      act(() => slider.dispatchEvent(new MouseEvent('mouseout', { bubbles: true })))
      expect(host.querySelectorAll('[data-rail-neighbor]')).toHaveLength(0)
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('doubles notch pitch and width without dropping notches', () => {
    const host = document.createElement('div')
    document.body.append(host)
    const root = createRoot(host)
    try {
      const indices = Array.from({ length: 200 }, (_, index) => index * 2)
      act(() => root.render(<MessageNavigationRail userMessageIndices={indices} startIndex={0} messages={[]} currentIndex={0} viewportRef={createRef()} busy={false} onNavigate={vi.fn()} />))
      const slider = host.querySelector<HTMLElement>('[role="slider"]')!
      expect(slider.getAttribute('aria-valuemax')).toBe('200')
      expect(slider.style.height).toBe('1592px')
      expect(slider.style.maxHeight).toBe('100%')
      const spine = slider.querySelector<HTMLElement>('span')!
      expect(spine.style.width).toBe('14px')
      expect(spine.style.backgroundImage).toContain('1.5px')
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
      expect((slider as HTMLElement).style.height).toBe('16px')
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


  it('preserves the focused unloaded keyboard target on pointer exit while the parent remains at its loaded tail', () => {
    const host = document.createElement('div')
    const external = document.createElement('button')
    external.textContent = 'Outside the rail'
    document.body.append(host, external)
    const root = createRoot(host)
    const onNavigate = vi.fn()
    const viewportRef = createRef<HTMLDivElement>()
    const messages = [{ id: '99999', sessionId: 'child', role: 'user' as const, content: 'Loaded tail question', createdAt: new Date() }]
    // SubagentTranscript keeps this currentIndex at the loaded tail until its owned history read settles.
    // This direct-props harness holds that pending state; it does not execute a history read.
    const render = (busy: boolean) => root.render(<MessageNavigationRail userMessageIndices={[0, 10, 99999]} startIndex={99999}
      messages={messages} currentIndex={99999} viewportRef={viewportRef} busy={busy} onNavigate={onNavigate} />)
    const targetValue = 'User message 1 of 3. Not loaded yet. Scroll up or click to load this part of the chat.'
    try {
      act(() => render(false))
      const slider = host.querySelector<HTMLElement>('[role="slider"]')!
      act(() => slider.focus())
      expect(document.activeElement).toBe(slider)
      expect(slider.getAttribute('aria-valuenow')).toBe('3')
      act(() => slider.dispatchEvent(new KeyboardEvent('keydown', { key: 'Home', bubbles: true })))
      expect(slider.getAttribute('aria-valuetext')).toBe(targetValue)
      expect(onNavigate.mock.calls).toEqual([[0]])
      act(() => render(true))
      expect(host.querySelector('[role="slider"]')).toBe(slider)
      expect(slider.getAttribute('aria-disabled')).toBe('true')
      expect(document.activeElement).toBe(slider)
      // React derives onMouseLeave from mouseout; a pointer exit does not move keyboard focus.
      act(() => slider.dispatchEvent(new MouseEvent('mouseout', { bubbles: true, relatedTarget: external })))
      expect(document.activeElement).toBe(slider)
      expect(onNavigate.mock.calls).toEqual([[0]])
      expect(slider.getAttribute('aria-valuenow')).toBe('1')
      expect(slider.getAttribute('aria-valuetext')).toBe(targetValue)
      expect(host.querySelector('[data-rail-target="0"]')).not.toBeNull()
      expect(host.querySelector('.conversation-rail-preview__label')?.textContent).toBe('User message 1 of 3')
      expect(host.querySelector('.conversation-rail-preview__title')?.textContent).toBe('Not loaded yet')
    } finally {
      act(() => root.unmount())
      host.remove()
      external.remove()
    }
  })

  it('clears a pending keyboard preview on actual blur without taking external focus or navigating again', () => {
    const host = document.createElement('div')
    const external = document.createElement('button')
    external.textContent = 'Outside the rail'
    document.body.append(host, external)
    const root = createRoot(host)
    const onNavigate = vi.fn()
    const viewportRef = createRef<HTMLDivElement>()
    const messages = [{ id: '99999', sessionId: 'child', role: 'user' as const, content: 'Loaded tail question', createdAt: new Date() }]
    const render = (busy: boolean) => root.render(<MessageNavigationRail userMessageIndices={[0, 10, 99999]} startIndex={99999}
      messages={messages} currentIndex={99999} viewportRef={viewportRef} busy={busy} onNavigate={onNavigate} />)
    try {
      act(() => render(false))
      const slider = host.querySelector<HTMLElement>('[role="slider"]')!
      act(() => slider.focus())
      act(() => slider.dispatchEvent(new KeyboardEvent('keydown', { key: 'Home', bubbles: true })))
      act(() => render(true))
      expect(slider.getAttribute('aria-valuenow')).toBe('1')
      expect(host.querySelector('.conversation-rail-preview')).not.toBeNull()
      act(() => external.focus())
      act(() => slider.dispatchEvent(new MouseEvent('mouseout', { bubbles: true, relatedTarget: external })))
      expect(document.activeElement).toBe(external)
      expect(host.querySelector('.conversation-rail-preview')).toBeNull()
      expect(slider.getAttribute('aria-valuenow')).toBe('3')
      expect(slider.getAttribute('aria-valuetext')).toBe('User message 3 of 3: Loaded tail question')
      expect(onNavigate.mock.calls).toEqual([[0]])
    } finally {
      act(() => root.unmount())
      host.remove()
      external.remove()
    }
  })

  it('follows a real external currentIndex change after blur without restoring a stale preview or moving external focus', () => {
    const host = document.createElement('div')
    const external = document.createElement('button')
    external.textContent = 'Outside the rail'
    document.body.append(host, external)
    const root = createRoot(host)
    const onNavigate = vi.fn()
    const viewportRef = createRef<HTMLDivElement>()
    const messages = [{ id: '99999', sessionId: 'child', role: 'user' as const, content: 'Loaded tail question', createdAt: new Date() }]
    const render = (currentIndex: number) => root.render(<MessageNavigationRail userMessageIndices={[0, 10, 99999]} startIndex={99999}
      messages={messages} currentIndex={currentIndex} viewportRef={viewportRef} busy={false} onNavigate={onNavigate} />)
    try {
      act(() => render(99999))
      const slider = host.querySelector<HTMLElement>('[role="slider"]')!
      act(() => slider.focus())
      act(() => slider.dispatchEvent(new KeyboardEvent('keydown', { key: 'Home', bubbles: true })))
      act(() => external.focus())
      act(() => render(10))
      expect(host.querySelector('[role="slider"]')).toBe(slider)
      expect(document.activeElement).toBe(external)
      expect(host.querySelector('.conversation-rail-preview')).toBeNull()
      expect(slider.getAttribute('aria-valuenow')).toBe('2')
      expect(slider.getAttribute('aria-valuetext')).toBe('User message 2 of 3. Not loaded yet. Scroll up or click to load this part of the chat.')
      expect(onNavigate.mock.calls).toEqual([[0]])
    } finally {
      act(() => root.unmount())
      host.remove()
      external.remove()
    }
  })
})
