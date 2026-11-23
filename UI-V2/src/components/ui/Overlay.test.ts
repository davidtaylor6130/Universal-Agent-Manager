import { act, createElement, useEffect } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { Overlay, scaledMs, usePresence } from './Overlay'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

afterEach(() => {
  vi.useRealTimers()
  vi.unstubAllGlobals()
})

const stubScale = (value: string) => vi.stubGlobal('getComputedStyle', () => ({ getPropertyValue: () => value }))

describe('scaledMs', () => {
  it('follows Appearance > Animation speed so exits are not cut short', () => {
    stubScale(' 1.5')
    expect(scaledMs(170)).toBe(255)
    stubScale('0.67')
    expect(scaledMs(100)).toBeCloseTo(67)
  })

  it('keeps the base length when no scale is set (Default)', () => {
    stubScale('')
    expect(scaledMs(170)).toBe(170)
  })
})

describe('usePresence', () => {
  it('stays mounted for the exit scaled by Animation speed', () => {
    vi.useFakeTimers()
    stubScale('1.5')
    vi.stubGlobal('matchMedia', (query: string) => ({ matches: false, media: query, addEventListener() {}, removeEventListener() {} }))
    const states: Array<{ mounted: boolean; closing: boolean }> = []
    const Probe = ({ open }: { open: boolean }) => { states.push(usePresence(open)); return null }
    const root = createRoot(document.createElement('div'))
    act(() => root.render(createElement(Probe, { open: true })))
    act(() => root.render(createElement(Probe, { open: false })))
    act(() => { vi.advanceTimersByTime(170) })
    expect(states.at(-1)).toEqual({ mounted: true, closing: true })
    act(() => { vi.advanceTimersByTime(85) })
    expect(states.at(-1)).toEqual({ mounted: false, closing: false })
    act(() => root.unmount())
  })
})

describe('Overlay dialog semantics and focus', () => {
  const setup = () => {
    const opener = document.createElement('button')
    opener.textContent = 'Open'
    const host = document.createElement('div')
    document.body.append(opener, host)
    opener.focus()
    return { opener, host, root: createRoot(host), cleanup: () => { opener.remove(); host.remove() } }
  }
  const field = (id: string, autoFocus = false) => createElement('input', { id, 'aria-label': id, ref: autoFocus ? (element: HTMLInputElement | null) => element?.focus() : undefined })

  it('names the layer as a modal dialog and focuses it when the content does not take focus', async () => {
    const { opener, root, cleanup } = setup()
    act(() => root.render(createElement(Overlay, { open: true, label: 'Pick a folder', describedBy: 'hint' }, createElement('p', { id: 'hint' }, 'Read only.'))))
    const layer = document.body.querySelector<HTMLElement>('.uam-overlay')!
    expect(layer.getAttribute('role')).toBe('dialog')
    expect(layer.getAttribute('aria-modal')).toBe('true')
    expect(layer.getAttribute('aria-label')).toBe('Pick a folder')
    expect(layer.getAttribute('aria-describedby')).toBe('hint')
    expect(document.activeElement).toBe(layer)
    await act(async () => root.render(createElement(Overlay, { open: false, label: 'Pick a folder' }, null)))
    expect(document.activeElement).toBe(opener)
    act(() => root.unmount())
    cleanup()
  })

  it('leaves dialog semantics to content that declares its own', () => {
    const { root, cleanup } = setup()
    act(() => root.render(createElement(Overlay, { open: true }, createElement('div', { role: 'dialog', 'aria-modal': 'true', 'aria-label': 'Own' }))))
    expect(document.body.querySelector('.uam-overlay')!.hasAttribute('role')).toBe(false)
    expect(document.body.querySelectorAll('[aria-modal="true"]')).toHaveLength(1)
    act(() => root.unmount())
    cleanup()
  })

  it('returns focus to the opener on close and on unmount, even without its own semantics', async () => {
    const { opener, root, cleanup } = setup()
    act(() => root.render(createElement(Overlay, { open: true }, field('name', true))))
    expect(document.activeElement?.id).toBe('name')
    await act(async () => root.render(createElement(Overlay, { open: false }, field('name'))))
    expect(document.activeElement).toBe(opener)
    act(() => root.render(createElement(Overlay, { open: true, label: 'Again' }, field('name', true))))
    expect(document.activeElement?.id).toBe('name')
    await act(async () => root.unmount())
    expect(document.activeElement).toBe(opener)
    cleanup()
  })

  it('uses returnFocusTo when the opener is gone, and never overrides focus the caller already moved', async () => {
    const { opener, root, cleanup } = setup()
    const fallback = document.createElement('button')
    document.body.append(fallback)
    act(() => root.render(createElement(Overlay, { open: true, label: 'Menu action', returnFocusTo: () => fallback }, field('name', true))))
    opener.remove()
    await act(async () => root.render(createElement(Overlay, { open: false, label: 'Menu action', returnFocusTo: () => fallback }, null)))
    expect(document.activeElement).toBe(fallback)

    const elsewhere = document.createElement('textarea')
    document.body.append(elsewhere)
    act(() => root.render(createElement(Overlay, { open: true, label: 'Menu action' }, field('name', true))))
    elsewhere.focus()
    await act(async () => root.unmount())
    expect(document.activeElement).toBe(elsewhere)
    fallback.remove()
    elsewhere.remove()
    cleanup()
  })

  it('lets the owner choose the return target, falling back only when focus would be lost', async () => {
    const { opener, root, cleanup } = setup()
    const trigger = document.createElement('button')
    document.body.append(trigger)
    // Like a chat's Goals window: in its own cleanup (which runs after the Overlay's) the owner refocuses its trigger
    // only when focus was lost, so an Overlay that restored first would have taken that choice away.
    function Owner({ open }: { open: boolean }) {
      useEffect(() => {
        if (!open) return
        return () => { const active = document.activeElement; if (!active || active === document.body || active.id === 'goal') trigger.focus() }
      }, [open])
      return createElement(Overlay, { open, label: 'Goals' }, field('goal', true))
    }
    act(() => root.render(createElement(Owner, { open: true })))
    expect(document.activeElement?.id).toBe('goal')
    await act(async () => root.render(createElement(Owner, { open: false })))
    expect(document.activeElement).toBe(trigger)
    expect(document.activeElement).not.toBe(opener)
    trigger.remove()
    await act(async () => root.unmount())
    cleanup()
  })

  it('keeps focus in a dialog that replaced this one as the next step', async () => {
    const { root, cleanup } = setup()
    act(() => root.render(createElement(Overlay, { key: 'form', open: true, label: 'New chat' }, field('name', true))))
    await act(async () => root.render(createElement(Overlay, { key: 'browser', open: true, label: 'Choose directory' }, field('path', true))))
    expect(document.activeElement?.id).toBe('path')
    expect(document.body.querySelector('[aria-label="Choose directory"]')!.contains(document.activeElement)).toBe(true)
    act(() => root.unmount())
    cleanup()
  })
})
