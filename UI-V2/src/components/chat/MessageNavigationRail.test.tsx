import { act, createRef } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { MessageNavigationRail, currentVisibleMessage, markAtOrBefore, messageIndexAtPointer } from './MessageNavigationRail'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

type Observed = { callback: IntersectionObserverCallback; targets: Set<Element>; options?: IntersectionObserverInit }

/** jsdom has no IntersectionObserver; this stand-in lets a test say which rows are on screen. */
function stubIntersectionObserver() {
  const observers: Observed[] = []
  vi.stubGlobal('IntersectionObserver', class {
    private entry: Observed
    constructor(callback: IntersectionObserverCallback, options?: IntersectionObserverInit) { this.entry = { callback, targets: new Set(), options }; observers.push(this.entry) }
    observe(target: Element) { this.entry.targets.add(target) }
    unobserve(target: Element) { this.entry.targets.delete(target) }
    disconnect() { this.entry.targets.clear() }
  })
  const show = (visible: Element[], hidden: Element[] = []) => {
    for (const observer of observers) {
      const entries = [...visible.map((target) => ({ target, isIntersecting: true })), ...hidden.map((target) => ({ target, isIntersecting: false }))]
        .filter((entry) => observer.targets.has(entry.target))
      if (entries.length) observer.callback(entries as unknown as IntersectionObserverEntry[], observer as unknown as IntersectionObserver)
    }
  }
  return { observers, show }
}

function transcript(roles: Array<'user' | 'assistant'>) {
  const viewport = document.createElement('div')
  const rows = roles.map((_, index) => {
    const row = document.createElement('div')
    row.dataset.messageIndex = String(index)
    viewport.append(row)
    return row
  })
  const messages = roles.map((role, index) => ({ id: String(index), sessionId: 'chat', role, content: `${role} ${index}`, createdAt: new Date() }))
  return { viewport, rows, messages, viewportRef: { current: viewport } }
}

describe('Message navigation rail visible range', () => {
  it('finds the last mark at or before a message', () => {
    expect(markAtOrBefore([0, 2, 4, 6], 0)).toBe(0)
    expect(markAtOrBefore([0, 2, 4, 6], 3)).toBe(1)
    expect(markAtOrBefore([0, 2, 4, 6], 6)).toBe(3)
    expect(markAtOrBefore([0, 2, 4, 6], 99)).toBe(3)
    expect(markAtOrBefore([5], 1)).toBe(0)
  })

  it('selects the first visible user message, or the one owning the topmost visible reply', () => {
    expect(currentVisibleMessage([0, 2, 4, 6], [3, 4, 5], false)).toBe(4)
    expect(currentVisibleMessage([0, 2, 4, 6], [3], false)).toBe(3)
    expect(markAtOrBefore([0, 2, 4, 6], currentVisibleMessage([0, 2, 4, 6], [3], false))).toBe(1)
  })

  it('selects the final user message at the bottom even when an earlier one is still visible', () => {
    // Final Message Selection: the earlier turn at the top used to win, so the rail showed the second-to-last mark.
    const indices = [0, 2, 4, 6]
    expect(markAtOrBefore(indices, currentVisibleMessage(indices, [4, 5, 6, 7], true))).toBe(3)
    expect(markAtOrBefore(indices, currentVisibleMessage(indices, [4, 5, 6, 7], false))).toBe(2)
    // A long final reply that hides its own question still belongs to the final mark.
    expect(markAtOrBefore(indices, currentVisibleMessage(indices, [7], true))).toBe(3)
  })
})

describe('Message navigation rail', () => {
  it('lights every visible user mark and selects the final one when the transcript end is on screen', () => {
    const { show } = stubIntersectionObserver()
    const { viewport, rows, messages, viewportRef } = transcript(['user', 'assistant', 'user', 'assistant', 'user', 'assistant'])
    const host = document.createElement('div')
    document.body.append(viewport, host)
    const root = createRoot(host)
    try {
      act(() => root.render(<MessageNavigationRail startIndex={0} messages={messages} currentIndex={5} viewportRef={viewportRef} busy={false} onNavigate={vi.fn()} />))
      const slider = host.querySelector('[role="slider"]')!
      act(() => show([rows[1], rows[2], rows[3]]))
      expect([...host.querySelectorAll<HTMLElement>('[data-rail-visible]')].map((mark) => mark.dataset.railVisible)).toEqual(['1'])
      expect(slider.getAttribute('aria-valuenow')).toBe('2')
      act(() => show([rows[0]]))
      expect([...host.querySelectorAll<HTMLElement>('[data-rail-visible]')].map((mark) => mark.dataset.railVisible)).toEqual(['0', '1'])
      expect(slider.getAttribute('aria-valuenow')).toBe('1')
      // Scrolled to the bottom: the second-to-last question is still at the top, the final turn below it.
      act(() => show([rows[2], rows[3], rows[4], rows[5]], [rows[0], rows[1]]))
      expect([...host.querySelectorAll<HTMLElement>('[data-rail-visible]')].map((mark) => mark.dataset.railVisible)).toEqual(['1', '2'])
      expect(slider.getAttribute('aria-valuenow')).toBe('3')
      expect(host.querySelector<HTMLElement>('[data-rail-target]')!.style.top).toBe('100%')
    } finally { act(() => root.unmount()); host.remove(); viewport.remove(); vi.unstubAllGlobals() }
  })

  it('observes rows that mount after the rail and ignores caller position changes while tracking', async () => {
    const { observers, show } = stubIntersectionObserver()
    const { viewport, rows, messages, viewportRef } = transcript(['user', 'assistant', 'user', 'assistant'])
    const host = document.createElement('div')
    document.body.append(viewport, host)
    const root = createRoot(host)
    const render = (currentIndex: number, list = messages) => root.render(<MessageNavigationRail startIndex={0} messages={list} currentIndex={currentIndex}
      viewportRef={viewportRef} busy={false} onNavigate={vi.fn()} />)
    try {
      act(() => render(3))
      act(() => show([rows[0], rows[1]]))
      const slider = host.querySelector('[role="slider"]')!
      expect(slider.getAttribute('aria-valuenow')).toBe('1')
      const late = document.createElement('div')
      late.dataset.messageIndex = '4'
      await act(async () => { viewport.append(late); await new Promise((resolve) => requestAnimationFrame(resolve)) })
      expect(observers[0].targets.has(late)).toBe(true)
      // A streamed message moves the caller's tail index, but the reader is still at the top.
      act(() => render(4, [...messages, { id: '4', sessionId: 'chat', role: 'user', content: 'Later', createdAt: new Date() }]))
      expect(slider.getAttribute('aria-valuenow')).toBe('1')
      await act(async () => { rows[0].remove(); await new Promise((resolve) => requestAnimationFrame(resolve)) })
      expect(observers[0].targets.has(rows[0])).toBe(false)
    } finally { act(() => root.unmount()); host.remove(); viewport.remove(); vi.unstubAllGlobals() }
  })

  it('does not count rows hidden under the floating composer as on screen', () => {
    const { observers, show } = stubIntersectionObserver()
    let resized: (() => void) | undefined
    vi.stubGlobal('ResizeObserver', class { constructor(callback: () => void) { resized = callback } observe() {}; unobserve() {}; disconnect() {} })
    const { viewport, rows, messages, viewportRef } = transcript(['user', 'assistant', 'user', 'assistant'])
    const host = document.createElement('div')
    document.body.append(viewport, host)
    const root = createRoot(host)
    const style = window.getComputedStyle
    vi.spyOn(window, 'getComputedStyle').mockImplementation((element) => element.classList.contains('conversation-rail')
      ? { bottom: '216px' } as CSSStyleDeclaration : style(element))
    try {
      act(() => root.render(<MessageNavigationRail startIndex={0} messages={messages} currentIndex={3} viewportRef={viewportRef} busy={false} onNavigate={vi.fn()} />))
      expect(observers).toHaveLength(1)
      expect(observers[0].options?.rootMargin).toBeUndefined()
      act(() => resized!())
      // A 200px composer (rail bottom 216px minus its 16px gap) shrinks the visible band; every row is observed again.
      expect(observers).toHaveLength(2)
      expect(observers[1].options).toMatchObject({ root: viewport, rootMargin: '0px 0px -200px 0px' })
      expect([...observers[1].targets]).toEqual(rows)
      act(() => show([rows[0], rows[1]]))
      expect(host.querySelector('[role="slider"]')!.getAttribute('aria-valuenow')).toBe('1')
      act(() => resized!())
      expect(observers).toHaveLength(2)
    } finally { act(() => root.unmount()); host.remove(); viewport.remove(); vi.restoreAllMocks(); vi.unstubAllGlobals() }
  })

  it('steps to the previous and next user message with keyboard-reachable buttons that keep focus while busy', () => {
    const { show } = stubIntersectionObserver()
    // Keyboard focus opens the Radix tooltip, which measures itself.
    vi.stubGlobal('ResizeObserver', class { observe() {}; unobserve() {}; disconnect() {} })
    const { viewport, rows, messages, viewportRef } = transcript(['user', 'assistant', 'user', 'assistant', 'user'])
    const host = document.createElement('div')
    document.body.append(viewport, host)
    const root = createRoot(host)
    const onNavigate = vi.fn()
    const render = (busy: boolean) => root.render(<MessageNavigationRail startIndex={0} messages={messages} currentIndex={4}
      viewportRef={viewportRef} busy={busy} onNavigate={onNavigate} />)
    try {
      act(() => render(false))
      act(() => show([rows[2]]))
      const previous = host.querySelector<HTMLButtonElement>('button[aria-label="Previous user message"]')!
      const next = host.querySelector<HTMLButtonElement>('button[aria-label="Next user message"]')!
      const slider = host.querySelector<HTMLElement>('[role="slider"]')!
      // Tab order runs previous, marks, next.
      expect(previous.compareDocumentPosition(slider) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy()
      expect(slider.compareDocumentPosition(next) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy()
      expect(previous.type).toBe('button')
      act(() => next.focus())
      expect(document.activeElement).toBe(next)
      act(() => next.click())
      expect(onNavigate).toHaveBeenLastCalledWith(4)
      expect(slider.getAttribute('aria-valuenow')).toBe('3')
      // The parent marks the jump busy; the button stays focused (a disabled button would drop focus) but inert.
      act(() => render(true))
      expect(document.activeElement).toBe(next)
      expect(next.disabled).toBe(false)
      expect(next.getAttribute('aria-disabled')).toBe('true')
      act(() => previous.click())
      expect(onNavigate).toHaveBeenCalledTimes(1)
      act(() => render(false))
      expect(next.getAttribute('aria-disabled')).toBe('true')
      act(() => previous.click())
      expect(onNavigate).toHaveBeenLastCalledWith(2)
      act(() => previous.click())
      expect(onNavigate).toHaveBeenLastCalledWith(0)
      expect(previous.getAttribute('aria-disabled')).toBe('true')
      act(() => previous.click())
      expect(onNavigate).toHaveBeenCalledTimes(3)
    } finally { act(() => root.unmount()); host.remove(); viewport.remove(); vi.unstubAllGlobals() }
  })

  it('keeps a jump target selected near the end until the reader scrolls, so Previous never stalls', () => {
    const { show } = stubIntersectionObserver()
    const { viewport, rows, messages, viewportRef } = transcript(['user', 'assistant', 'user', 'assistant', 'user', 'assistant'])
    const host = document.createElement('div')
    document.body.append(viewport, host)
    const root = createRoot(host)
    const onNavigate = vi.fn()
    try {
      act(() => root.render(<MessageNavigationRail startIndex={0} messages={messages} currentIndex={5} viewportRef={viewportRef} busy={false} onNavigate={onNavigate} />))
      const slider = host.querySelector('[role="slider"]')!
      const previous = host.querySelector<HTMLButtonElement>('button[aria-label="Previous user message"]')!
      act(() => show([rows[2], rows[3], rows[4], rows[5]]))
      expect(slider.getAttribute('aria-valuenow')).toBe('3')
      act(() => previous.click())
      expect(onNavigate).toHaveBeenLastCalledWith(2)
      // The target scrolls into view but the end stays on screen; the jump target still holds the selection.
      act(() => show([rows[1]]))
      expect(slider.getAttribute('aria-valuenow')).toBe('2')
      act(() => previous.click())
      expect(onNavigate).toHaveBeenLastCalledWith(0)
      expect(slider.getAttribute('aria-valuenow')).toBe('1')
      // Reader input hands selection back to what is on screen.
      act(() => { viewport.dispatchEvent(new WheelEvent('wheel', { deltaY: 40 })) })
      expect(slider.getAttribute('aria-valuenow')).toBe('3')
    } finally { act(() => root.unmount()); host.remove(); viewport.remove(); vi.unstubAllGlobals() }
  })

  it('releases a seen jump target once it leaves the screen', () => {
    const { show } = stubIntersectionObserver()
    const { viewport, rows, messages, viewportRef } = transcript(['user', 'assistant', 'user', 'assistant', 'user', 'assistant'])
    const host = document.createElement('div')
    document.body.append(viewport, host)
    const root = createRoot(host)
    try {
      act(() => root.render(<MessageNavigationRail startIndex={0} messages={messages} currentIndex={5} viewportRef={viewportRef} busy={false} onNavigate={vi.fn()} />))
      const slider = host.querySelector<HTMLElement>('[role="slider"]')!
      act(() => show([rows[0], rows[1]]))
      act(() => slider.dispatchEvent(new KeyboardEvent('keydown', { key: 'End', bubbles: true })))
      act(() => slider.dispatchEvent(new FocusEvent('focusout', { bubbles: true })))
      act(() => show([rows[3], rows[4]], [rows[0], rows[1]]))
      expect(slider.getAttribute('aria-valuenow')).toBe('3')
      act(() => show([rows[2], rows[3]], [rows[4]]))
      expect(slider.getAttribute('aria-valuenow')).toBe('2')
    } finally { act(() => root.unmount()); host.remove(); viewport.remove(); vi.unstubAllGlobals() }
  })

  it('treats the end of an older loaded page as mid-chat and drops rows that lose their index', async () => {
    const { observers, show } = stubIntersectionObserver()
    const { viewport, rows, messages, viewportRef } = transcript(['user', 'assistant', 'user', 'assistant'])
    const host = document.createElement('div')
    document.body.append(viewport, host)
    const root = createRoot(host)
    try {
      // The full history has a later user message (index 40) that this page does not contain.
      act(() => root.render(<MessageNavigationRail userMessageIndices={[0, 2, 40]} startIndex={0} messages={messages} currentIndex={3}
        viewportRef={viewportRef} busy={false} onNavigate={vi.fn()} />))
      const slider = host.querySelector('[role="slider"]')!
      act(() => show([rows[0], rows[1], rows[2], rows[3]]))
      expect(slider.getAttribute('aria-valuenow')).toBe('1')
      await act(async () => { rows[0].removeAttribute('data-message-index'); await new Promise((resolve) => requestAnimationFrame(resolve)) })
      expect(observers[0].targets.has(rows[0])).toBe(false)
      expect([...host.querySelectorAll<HTMLElement>('[data-rail-visible]')].map((mark) => mark.dataset.railVisible)).toEqual(['1'])
      expect(slider.getAttribute('aria-valuenow')).toBe('2')
    } finally { act(() => root.unmount()); host.remove(); viewport.remove(); vi.unstubAllGlobals() }
  })

  it('moves between marks with the arrow keys and confirms with Enter', () => {
    const host = document.createElement('div')
    document.body.append(host)
    const root = createRoot(host)
    const onNavigate = vi.fn()
    try {
      act(() => root.render(<MessageNavigationRail userMessageIndices={[0, 4, 9]} startIndex={0} messages={[]} currentIndex={4} viewportRef={createRef()} busy={false} onNavigate={onNavigate} />))
      const slider = host.querySelector<HTMLElement>('[role="slider"]')!
      expect(slider.tabIndex).toBe(0)
      act(() => slider.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })))
      expect(onNavigate).toHaveBeenLastCalledWith(9)
      act(() => slider.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowUp', bubbles: true })))
      expect(onNavigate).toHaveBeenLastCalledWith(4)
      act(() => slider.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })))
      expect(onNavigate).toHaveBeenLastCalledWith(4)
      expect(onNavigate).toHaveBeenCalledTimes(3)
    } finally { act(() => root.unmount()); host.remove() }
  })

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
