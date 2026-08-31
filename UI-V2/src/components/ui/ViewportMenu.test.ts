import { act, createElement } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { placeViewportMenu, VIEWPORT_MENU_Z_INDEX, ViewportMenu } from './ViewportMenu'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

afterEach(() => {
  vi.restoreAllMocks()
  vi.unstubAllGlobals()
})

describe('placeViewportMenu', () => {
  it('keeps viewport menus above application dialogs', () => {
    expect(VIEWPORT_MENU_Z_INDEX).toBeGreaterThan(1000)
  })

  it('flips above a bottom-edge anchor and clamps both horizontal edges', () => {
    expect(placeViewportMenu(
      { left: 290, right: 290, top: 190, bottom: 190 },
      { width: 120, height: 100 },
      { width: 320, height: 200 },
    )).toEqual({ left: 192, top: 86 })

    expect(placeViewportMenu(
      { left: -20, right: -20, top: 20, bottom: 20 },
      { width: 120, height: 100 },
      { width: 320, height: 200 },
    ).left).toBe(8)
  })

  it('opens beside an anchor and flips at the right edge', () => {
    expect(placeViewportMenu(
      { left: 280, right: 300, top: 40, bottom: 70 },
      { width: 120, height: 80 },
      { width: 320, height: 200 },
      'right',
    )).toEqual({ left: 156, top: 40 })
  })

  it('repositions when the menu or anchor size changes', () => {
    let resizeCallback: ResizeObserverCallback | undefined
    const observe = vi.fn()
    const disconnect = vi.fn()
    vi.stubGlobal('ResizeObserver', class {
      constructor(callback: ResizeObserverCallback) {
        resizeCallback = callback
      }
      observe = observe
      unobserve() {}
      disconnect = disconnect
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    const anchorRef = { current: document.createElement('button') }
    document.body.appendChild(anchorRef.current)
    let anchorTop = 300
    let menuHeight = 120
    vi.spyOn(HTMLElement.prototype, 'getBoundingClientRect').mockImplementation(function () {
      if (this === anchorRef.current) {
        return { left: 100, right: 180, top: anchorTop, bottom: anchorTop + 30, width: 80, height: 30, x: 100, y: anchorTop, toJSON: () => ({}) }
      }
      if ((this as HTMLElement).hasAttribute('data-viewport-menu')) {
        return { left: 0, right: 160, top: 0, bottom: menuHeight, width: 160, height: menuHeight, x: 0, y: 0, toJSON: () => ({}) }
      }
      return { left: 0, right: 0, top: 0, bottom: 0, width: 0, height: 0, x: 0, y: 0, toJSON: () => ({}) }
    })

    act(() => root.render(createElement(
      ViewportMenu,
      { anchorRef, side: 'top', manageFocus: false },
      createElement('div', null, 'Commands'),
    )))

    const menu = document.body.querySelector('[data-viewport-menu]') as HTMLElement
    expect(menu.style.top).toBe('176px')
    expect(observe).toHaveBeenCalledWith(menu)
    expect(observe).toHaveBeenCalledWith(anchorRef.current)

    menuHeight = 40
    act(() => resizeCallback?.([], {} as ResizeObserver))
    expect(menu.style.top).toBe('256px')

    anchorTop = 280
    act(() => resizeCallback?.([], {} as ResizeObserver))
    expect(menu.style.top).toBe('236px')

    act(() => root.unmount())
    expect(disconnect).toHaveBeenCalledTimes(1)
    anchorRef.current.remove()
    host.remove()
  })

  it('manages focus and Escape dismissal for menu popups', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    const anchorRef = { current: document.createElement('button') }
    anchorRef.current.textContent = 'Open'
    document.body.appendChild(anchorRef.current)
    anchorRef.current.focus()
    const onRequestClose = vi.fn()

    act(() => root.render(createElement(
      ViewportMenu,
      { anchorRef, role: 'menu', onRequestClose },
      createElement('button', { role: 'menuitem' }, 'First'),
      createElement('button', { role: 'menuitem' }, 'Second'),
    )))

    expect(document.activeElement?.textContent).toBe('First')
    act(() => document.activeElement?.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })))
    expect(document.activeElement?.textContent).toBe('Second')
    act(() => document.activeElement?.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(onRequestClose).toHaveBeenCalledTimes(1)
    expect(document.activeElement).toBe(anchorRef.current)

    act(() => root.unmount())
    anchorRef.current.remove()
    host.remove()
  })
})
