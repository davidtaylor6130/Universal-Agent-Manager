import { act, createElement } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { placeViewportMenu, VIEWPORT_MENU_Z_INDEX, ViewportMenu } from './ViewportMenu'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

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
